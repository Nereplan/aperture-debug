#pragma once

#if !defined(_WIN32)

#include "LinuxWIC.h"
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>

// ============================================================================
// CRT Extensions & Secure String Functions
// ============================================================================

inline int _stricmp(const char* a, const char* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return ::strcasecmp(a, b);
}

inline int _strnicmp(const char* a, const char* b, size_t n) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return ::strncasecmp(a, b, n);
}

inline int _wcsicmp(const wchar_t* a, const wchar_t* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return ::wcscasecmp(a, b);
}

inline int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return ::wcsncasecmp(a, b, n);
}

inline int wcscpy_s(wchar_t* dst, size_t dstSize, const wchar_t* src) {
    if (!dst || dstSize == 0) return EINVAL;
    if (!src) { dst[0] = L'\0'; return EINVAL; }
    size_t len = std::wcslen(src);
    if (len >= dstSize) {
        std::wmemcpy(dst, src, dstSize - 1);
        dst[dstSize - 1] = L'\0';
        return ERANGE;
    }
    std::wmemcpy(dst, src, len + 1);
    return 0;
}

template <size_t N>
inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src) {
    return wcscpy_s(dst, N, src);
}

inline int wcsncpy_s(wchar_t* dst, size_t dstSize, const wchar_t* src, size_t count) {
    if (!dst || dstSize == 0) return EINVAL;
    if (!src) { dst[0] = L'\0'; return 0; }
    size_t srcLen = std::wcslen(src);
    size_t copyLen = (count == _TRUNCATE) ? std::min(srcLen, dstSize - 1) : std::min(srcLen, count);
    if (copyLen >= dstSize) {
        copyLen = dstSize - 1;
    }
    std::wmemcpy(dst, src, copyLen);
    dst[copyLen] = L'\0';
    return 0;
}

template <size_t N>
inline int wcsncpy_s(wchar_t (&dst)[N], const wchar_t* src, size_t count) {
    return wcsncpy_s(dst, N, src, count);
}

inline int strcpy_s(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) return EINVAL;
    if (!src) { dst[0] = '\0'; return EINVAL; }
    size_t len = std::strlen(src);
    if (len >= dstSize) {
        std::memcpy(dst, src, dstSize - 1);
        dst[dstSize - 1] = '\0';
        return ERANGE;
    }
    std::memcpy(dst, src, len + 1);
    return 0;
}

template <size_t N>
inline int strcpy_s(char (&dst)[N], const char* src) {
    return strcpy_s(dst, N, src);
}

inline int strncpy_s(char* dst, size_t dstSize, const char* src, size_t count) {
    if (!dst || dstSize == 0) return EINVAL;
    if (!src) { dst[0] = '\0'; return 0; }
    size_t srcLen = std::strlen(src);
    size_t copyLen = (count == _TRUNCATE) ? std::min(srcLen, dstSize - 1) : std::min(srcLen, count);
    if (copyLen >= dstSize) copyLen = dstSize - 1;
    std::memcpy(dst, src, copyLen);
    dst[copyLen] = '\0';
    return 0;
}

template <size_t N>
inline int strncpy_s(char (&dst)[N], const char* src, size_t count) {
    return strncpy_s(dst, N, src, count);
}

inline int strcat_s(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0 || !src) return EINVAL;
    size_t curLen = std::strlen(dst);
    if (curLen >= dstSize) return ERANGE;
    return strcpy_s(dst + curLen, dstSize - curLen, src);
}

template <size_t N>
inline int strcat_s(char (&dst)[N], const char* src) {
    return strcat_s(dst, N, src);
}

inline int wcscat_s(wchar_t* dst, size_t dstSize, const wchar_t* src) {
    if (!dst || dstSize == 0 || !src) return EINVAL;
    size_t curLen = std::wcslen(dst);
    if (curLen >= dstSize) return ERANGE;
    return wcscpy_s(dst + curLen, dstSize - curLen, src);
}

template <size_t N>
inline int wcscat_s(wchar_t (&dst)[N], const wchar_t* src) {
    return wcscat_s(dst, N, src);
}

namespace _LinuxFormatDetail {
inline std::wstring FixWPrintfFormat(const wchar_t* fmt) {
    if (!fmt) return L"";
    std::wstring out;
    out.reserve(std::wcslen(fmt) + 8);
    for (size_t i = 0; fmt[i] != L'\0'; ++i) {
        if (fmt[i] == L'%') {
            out.push_back(L'%');
            ++i;
            if (fmt[i] == L'%') {
                out.push_back(L'%');
                continue;
            }
            while (fmt[i] != L'\0' && (std::iswdigit(fmt[i]) || fmt[i] == L'-' || fmt[i] == L'+' || fmt[i] == L' ' || fmt[i] == L'#' || fmt[i] == L'.' || fmt[i] == L'*')) {
                out.push_back(fmt[i]);
                ++i;
            }
            if (fmt[i] == L's') {
                out += L"ls";
            } else if (fmt[i] == L'S') {
                out += L"s";
            } else if (fmt[i] == L'I' && fmt[i + 1] == L'6' && fmt[i + 2] == L'4') {
                out += L"ll";
                i += 2;
            } else if (fmt[i] != L'\0') {
                out.push_back(fmt[i]);
            } else {
                break;
            }
        } else {
            out.push_back(fmt[i]);
        }
    }
    return out;
}
} // namespace _LinuxFormatDetail

template <typename... Args>
inline int swprintf_s(wchar_t* buffer, size_t sizeOfBuffer, const wchar_t* format, Args... args) {
    if (!buffer || sizeOfBuffer == 0 || !format) return -1;
    std::wstring fixed = _LinuxFormatDetail::FixWPrintfFormat(format);
    int r = std::swprintf(buffer, sizeOfBuffer, fixed.c_str(), args...);
    buffer[sizeOfBuffer - 1] = L'\0';
    return r;
}

template <size_t N, typename... Args>
inline int swprintf_s(wchar_t (&buffer)[N], const wchar_t* format, Args... args) {
    return swprintf_s(buffer, N, format, args...);
}

template <typename... Args>
inline int _snwprintf_s(wchar_t* buffer, size_t sizeOfBuffer, size_t /*count*/, const wchar_t* format, Args... args) {
    return swprintf_s(buffer, sizeOfBuffer, format, args...);
}

template <size_t N, typename... Args>
inline int _snwprintf_s(wchar_t (&buffer)[N], size_t /*count*/, const wchar_t* format, Args... args) {
    return swprintf_s(buffer, N, format, args...);
}

template <typename... Args>
inline int sprintf_s(char* buffer, size_t sizeOfBuffer, const char* format, Args... args) {
    if (!buffer || sizeOfBuffer == 0 || !format) return -1;
    int r = std::snprintf(buffer, sizeOfBuffer, format, args...);
    buffer[sizeOfBuffer - 1] = '\0';
    return r;
}

template <size_t N, typename... Args>
inline int sprintf_s(char (&buffer)[N], const char* format, Args... args) {
    return sprintf_s(buffer, N, format, args...);
}

inline int gmtime_s(struct tm* tmDest, const time_t* sourceTime) {
    if (!tmDest || !sourceTime) return EINVAL;
    return ::gmtime_r(sourceTime, tmDest) ? 0 : EINVAL;
}

inline int localtime_s(struct tm* tmDest, const time_t* sourceTime) {
    if (!tmDest || !sourceTime) return EINVAL;
    return ::localtime_r(sourceTime, tmDest) ? 0 : EINVAL;
}

inline time_t _mkgmtime(struct tm* timeptr) {
    if (!timeptr) return (time_t)-1;
    return ::timegm(timeptr);
}

FILE* _wfopen(const wchar_t* filename, const wchar_t* mode);
int _wfopen_s(FILE** pFile, const wchar_t* filename, const wchar_t* mode);
wchar_t* _wfullpath(wchar_t* absPath, const wchar_t* relPath, size_t maxLength);

// ============================================================================
// Atomic / Interlocked Functions
// ============================================================================

inline LONG InterlockedIncrement(volatile LONG* addend) {
    return __sync_add_and_fetch(addend, 1);
}
inline LONG InterlockedDecrement(volatile LONG* addend) {
    return __sync_sub_and_fetch(addend, 1);
}
inline LONG InterlockedExchange(volatile LONG* target, LONG value) {
    return __sync_lock_test_and_set(target, value);
}
inline LONG InterlockedCompareExchange(volatile LONG* destination, LONG exchange, LONG comperand) {
    return __sync_val_compare_and_swap(destination, comperand, exchange);
}
inline LONGLONG InterlockedIncrement64(volatile LONGLONG* addend) {
    return __sync_add_and_fetch(addend, 1);
}
inline LONGLONG InterlockedExchange64(volatile LONGLONG* target, LONGLONG value) {
    return __sync_lock_test_and_set(target, value);
}
inline LONGLONG InterlockedCompareExchange64(volatile LONGLONG* destination, LONGLONG exchange, LONGLONG comperand) {
    return __sync_val_compare_and_swap(destination, comperand, exchange);
}

// ============================================================================
// Win32 Constants (Error Codes, File, Window, Input, Messages, Virtual Keys)
// ============================================================================

#define ERROR_SUCCESS 0L
#define ERROR_INVALID_FUNCTION 1L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_INVALID_DATA 13L
#define ERROR_OUTOFMEMORY 14L
#define ERROR_NO_MORE_FILES 18L
#define ERROR_SHARING_VIOLATION 32L
#define ERROR_HANDLE_EOF 38L
#define ERROR_FILE_EXISTS 80L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_BROKEN_PIPE 109L
#define ERROR_DISK_FULL 112L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_MOD_NOT_FOUND 126L
#define ERROR_PROC_NOT_FOUND 127L
#define ERROR_DIR_NOT_EMPTY 145L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_MORE_DATA 234L
#define ERROR_WAIT_TIMEOUT 258L
#define ERROR_NO_MORE_ITEMS 259L
#define ERROR_DIRECTORY 267L
#define ERROR_Operation_ABORTED 995L
#define ERROR_OPERATION_ABORTED 995L
#define ERROR_IO_PENDING 997L
#define ERROR_NOT_FOUND 1168L
#define ERROR_CANCELLED 1223L
#define ERROR_WRITE_FAULT 29L
#define ERROR_FILE_TOO_LARGE 223L
#define ERROR_TIMEOUT 1460L
#define EXCEPTION_CONTINUE_SEARCH 0
#define FILE_READ_ATTRIBUTES 0x0080
#define FILE_WRITE_ATTRIBUTES 0x0100
#define SYNCHRONIZE 0x00100000L

#define CP_ACP 0
#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 0x00000008
#define WC_ERR_INVALID_CHARS 0x00000080

#define CSTR_LESS_THAN 1
#define CSTR_EQUAL 2
#define CSTR_GREATER_THAN 3
#define LCMAP_LOWERCASE 0x00000100
#define LCMAP_UPPERCASE 0x00000200
#define LCMAP_LINGUISTIC_CASING 0x01000000
#define LOCALE_NAME_USER_DEFAULT nullptr
#define LOCALE_NAME_INVARIANT L""
#define LOCALE_SISO639LANGNAME 0x00000059

// File & Directory constants
#define GENERIC_READ 0x80000000UL
#define GENERIC_WRITE 0x40000000UL
#define GENERIC_EXECUTE 0x20000000UL
#define GENERIC_ALL 0x10000000UL

#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004

#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5

#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_SYSTEM 0x00000004
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020
#define FILE_ATTRIBUTE_DEVICE 0x00000040
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400

#define FILE_FLAG_WRITE_THROUGH 0x80000000
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000

#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2

#define MOVEFILE_REPLACE_EXISTING 0x00000001
#define MOVEFILE_COPY_ALLOWED 0x00000002
#define MOVEFILE_WRITE_THROUGH 0x00000008

#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003L
#define IO_REPARSE_TAG_SYMLINK 0xA000000CL

// Sync & Wait constants
#define INFINITE 0xFFFFFFFFUL
#define WAIT_OBJECT_0 0x00000000UL
#define WAIT_ABANDONED_0 0x00000080UL
#define WAIT_TIMEOUT 258UL
#define WAIT_FAILED 0xFFFFFFFFUL
#define QS_ALLINPUT 0x04FFUL
#define MWMO_INPUTAVAILABLE 0x0004
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#define TIMER_MODIFY_STATE 0x0002
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)

#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_BASE_PRIORITY_LOWRT 15
#define THREAD_MODE_BACKGROUND_BEGIN 0x00010000
#define THREAD_MODE_BACKGROUND_END 0x00020000

#define ES_CONTINUOUS 0x80000000UL
#define ES_SYSTEM_REQUIRED 0x00000001UL
#define ES_DISPLAY_REQUIRED 0x00000002UL

// Window Messages
#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_SETREDRAW 0x000B
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUERYENDSESSION 0x0011
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SYSCOLORCHANGE 0x0015
#define WM_ENDSESSION 0x0016
#define WM_SHOWWINDOW 0x0018
#define WM_WININICHANGE 0x001A
#define WM_SETTINGCHANGE WM_WININICHANGE
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_MOUSEACTIVATE 0x0021
#define WM_GETMINMAXINFO 0x0024
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_COPYDATA 0x004A
#define WM_NOTIFY 0x004E
#define WM_HELP 0x0053
#define WM_CONTEXTMENU 0x007B
#define WM_STYLECHANGED 0x007D
#define WM_DISPLAYCHANGE 0x007E
#define WM_GETICON 0x007F
#define WM_SETICON 0x0080
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCPAINT 0x0085
#define WM_NCACTIVATE 0x0086
#define WM_GETDLGCODE 0x0087
#define WM_SYNCPAINT 0x0088
#define WM_NCMOUSEMOVE 0x00A0
#define WM_NCLBUTTONDOWN 0x00A1
#define WM_NCLBUTTONUP 0x00A2
#define WM_NCLBUTTONDBLCLK 0x00A3
#define WM_NCRBUTTONDOWN 0x00A4
#define WM_NCRBUTTONUP 0x00A5
#define WM_NCMOUSEHOVER 0x02A0
#define WM_NCMOUSELEAVE 0x02A2
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_DEADCHAR 0x0103
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_UNICHAR 0x0109
#define WM_IME_STARTCOMPOSITION 0x010D
#define WM_IME_ENDCOMPOSITION 0x010E
#define WM_IME_COMPOSITION 0x010F
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_HSCROLL 0x0114
#define WM_VSCROLL 0x0115
#define WM_INITMENU 0x0116
#define WM_INITMENUPOPUP 0x0117
#define WM_MENUSELECT 0x011F
#define WM_ENTERIDLE 0x0121
#define WM_CHANGEUISTATE 0x0127
#define WM_UPDATEUISTATE 0x0128
#define WM_QUERYUISTATE 0x0129
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MBUTTONDBLCLK 0x0209
#define WM_MOUSEWHEEL 0x020A
#define WM_XBUTTONDOWN 0x020B
#define WM_XBUTTONUP 0x020C
#define WM_XBUTTONDBLCLK 0x020D
#define WM_MOUSEHWHEEL 0x020E
#define WM_PARENTNOTIFY 0x0210
#define WM_ENTERMENULOOP 0x0211
#define WM_EXITMENULOOP 0x0212
#define WM_SIZING 0x0214
#define WM_CAPTURECHANGED 0x0215
#define WM_MOVING 0x0216
#define WM_POWERBROADCAST 0x0218
#define WM_DEVICECHANGE 0x0219
#define WM_ENTERSIZEMOVE 0x0231
#define WM_EXITSIZEMOVE 0x0232
#define WM_DROPFILES 0x0233
#define WM_TOUCH 0x0240
#define WM_POINTERDOWN 0x0246
#define WM_POINTERUP 0x0247
#define WM_POINTERUPDATE 0x0245
#define WM_MOUSEHOVER 0x02A1
#define WM_MOUSELEAVE 0x02A3
#define WM_WTSSESSION_CHANGE 0x02B1
#define WM_DPICHANGED 0x02E0
#define WM_CUT 0x0300
#define WM_COPY 0x0301
#define WM_PASTE 0x0302
#define WM_CLEAR 0x0303
#define WM_UNDO 0x0304
#define WM_HOTKEY 0x0312
#define WM_APPCOMMAND 0x0319
#define WM_THEMECHANGED 0x031A
#define WM_DWMCOMPOSITIONCHANGED 0x031E
#define WM_GETOBJECT 0x003D
#define WM_USER 0x0400
#define WM_APP 0x8000

// Virtual Keys
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_CANCEL 0x03
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_CLEAR 0x0C
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SELECT 0x29
#define VK_PRINT 0x2A
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_HELP 0x2F
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_APPS 0x5D
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_VOLUME_MUTE 0xAD
#define VK_VOLUME_DOWN 0xAE
#define VK_VOLUME_UP 0xAF
#define VK_MEDIA_NEXT_TRACK 0xB0
#define VK_MEDIA_PREV_TRACK 0xB1
#define VK_MEDIA_STOP 0xB2
#define VK_MEDIA_PLAY_PAUSE 0xB3
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE

#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002
#define MK_SHIFT 0x0004
#define MK_CONTROL 0x0008
#define MK_MBUTTON 0x0010
#define MK_XBUTTON1 0x0020
#define MK_XBUTTON2 0x0040
#define WHEEL_DELTA 120
#define XBUTTON1 0x0001
#define XBUTTON2 0x0002

#define APPCOMMAND_MEDIA_NEXTTRACK 11
#define APPCOMMAND_MEDIA_PREVIOUSTRACK 12
#define APPCOMMAND_MEDIA_STOP 13
#define APPCOMMAND_MEDIA_PLAY_PAUSE 14
#define APPCOMMAND_MEDIA_PLAY 46
#define APPCOMMAND_MEDIA_PAUSE 47
#define GET_APPCOMMAND_LPARAM(lParam) ((short)(HIWORD(lParam) & ~0xF000))

// Window Styles & HitTest
#define WS_OVERLAPPED 0x00000000L
#define WS_POPUP 0x80000000L
#define WS_CHILD 0x40000000L
#define WS_MINIMIZE 0x20000000L
#define WS_VISIBLE 0x10000000L
#define WS_DISABLED 0x08000000L
#define WS_CLIPSIBLINGS 0x04000000L
#define WS_CLIPCHILDREN 0x02000000L
#define WS_MAXIMIZE 0x01000000L
#define WS_CAPTION 0x00C00000L
#define WS_BORDER 0x00800000L
#define WS_DLGFRAME 0x00400000L
#define WS_VSCROLL 0x00200000L
#define WS_HSCROLL 0x00100000L
#define WS_SYSMENU 0x00080000L
#define WS_THICKFRAME 0x00040000L
#define WS_GROUP 0x00020000L
#define WS_TABSTOP 0x00010000L
#define WS_MINIMIZEBOX 0x00020000L
#define WS_MAXIMIZEBOX 0x00010000L
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)

#define WS_EX_DLGMODALFRAME 0x00000001L
#define WS_EX_NOPARENTNOTIFY 0x00000004L
#define WS_EX_TOPMOST 0x00000008L
#define WS_EX_ACCEPTFILES 0x00000010L
#define WS_EX_TRANSPARENT 0x00000020L
#define WS_EX_TOOLWINDOW 0x00000080L
#define WS_EX_WINDOWEDGE 0x00000100L
#define WS_EX_CLIENTEDGE 0x00000200L
#define WS_EX_STATICEDGE 0x00020000L
#define WS_EX_APPWINDOW 0x00040000L
#define WS_EX_LAYERED 0x00080000L
#define WS_EX_NOACTIVATE 0x08000000L
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L

#define CS_VREDRAW 0x0001
#define CS_HREDRAW 0x0002
#define CS_DBLCLKS 0x0008
#define CS_OWNDC 0x0020
#define CW_USEDEFAULT ((int)0x80000000)

#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_NORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_MAXIMIZE 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA 8
#define SW_RESTORE 9
#define SW_SHOWDEFAULT 10

#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOREDRAW 0x0008
#define SWP_NOACTIVATE 0x0010
#define SWP_FRAMECHANGED 0x0020
#define SWP_SHOWWINDOW 0x0040
#define SWP_HIDEWINDOW 0x0080
#define SWP_NOCOPYBITS 0x0100
#define SWP_NOOWNERZORDER 0x0200
#define SWP_NOSENDCHANGING 0x0400

#define HWND_TOP ((HWND)0)
#define HWND_BOTTOM ((HWND)1)
#define HWND_TOPMOST ((HWND)(intptr_t)-1)
#define HWND_NOTOPMOST ((HWND)(intptr_t)-2)

#define GWLP_WNDPROC (-4)
#define GWLP_HINSTANCE (-6)
#define GWLP_HWNDPARENT (-8)
#define GWLP_USERDATA (-21)
#define GWLP_ID (-12)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)

#define SIZE_RESTORED 0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2
#define SIZE_MAXSHOW 3
#define SIZE_MAXHIDE 4

#define WA_INACTIVE 0
#define WA_ACTIVE 1
#define WA_CLICKACTIVE 2

#define HTTERROR (-2)
#define HTTRANSPARENT (-1)
#define HTNOWHERE 0
#define HTCLIENT 1
#define HTCAPTION 2
#define HTSYSMENU 3
#define HTGROWBOX 4
#define HTSIZE HTGROWBOX
#define HTMENU 5
#define HTHSCROLL 6
#define HTVSCROLL 7
#define HTMINBUTTON 8
#define HTMAXBUTTON 9
#define HTLEFT 10
#define HTRIGHT 11
#define HTTOP 12
#define HTTOPLEFT 13
#define HTTOPRIGHT 14
#define HTBOTTOM 15
#define HTBOTTOMLEFT 16
#define HTBOTTOMRIGHT 17
#define HTBORDER 18
#define HTCLOSE 20
#define HTHELP 21

#define SC_SIZE 0xF000
#define SC_MOVE 0xF010
#define SC_MINIMIZE 0xF020
#define SC_MAXIMIZE 0xF030
#define SC_CLOSE 0xF060
#define SC_KEYMENU 0xF100
#define SC_RESTORE 0xF120
#define SC_SCREENSAVE 0xF140
#define SC_MONITORPOWER 0xF170

#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXVSCROLL 2
#define SM_CYHSCROLL 3
#define SM_CYCAPTION 4
#define SM_CXBORDER 5
#define SM_CYBORDER 6
#define SM_CXICON 11
#define SM_CYICON 12
#define SM_CXCURSOR 13
#define SM_CYCURSOR 14
#define SM_CXFULLSCREEN 16
#define SM_CYFULLSCREEN 17
#define SM_CXMINTRACK 34
#define SM_CYMINTRACK 35
#define SM_CXDOUBLECLK 36
#define SM_CYDOUBLECLK 37
#define SM_CXICONSPACING 38
#define SM_CYICONSPACING 39
#define SM_CXSMICON 49
#define SM_CYSMICON 50
#define SM_XVIRTUALSCREEN 76
#define SM_YVIRTUALSCREEN 77
#define SM_CXVIRTUALSCREEN 78
#define SM_CYVIRTUALSCREEN 79
#define SM_CMONITORS 80
#define SM_CXSIZEFRAME 32
#define SM_CYSIZEFRAME 33
#define SM_CXPADDEDBORDER 92
#define SM_DIGITIZER 94

#define SPI_GETWORKAREA 0x0030
#define SPI_GETWHEELSCROLLLINES 0x0068
#define SPI_GETWHEELSCROLLCHARS 0x006C
#define SPI_GETCLIENTAREAANIMATION 0x1042
#define SPI_GETHIGHCONTRAST 0x0042
#define SPI_GETMENUANIMATION 0x1002
#define SPI_GETMENUFADE 0x1012
#define SPI_GETSCREENREADER 0x0046

#define HKEY_CLASSES_ROOT ((HKEY)(ULONG_PTR)0x80000000)
#define HKEY_CURRENT_USER ((HKEY)(ULONG_PTR)0x80000001)
#define HKEY_LOCAL_MACHINE ((HKEY)(ULONG_PTR)0x80000002)
#define RRF_RT_REG_DWORD 0x00000010
#define RRF_RT_REG_SZ 0x00000002
#define RRF_RT_ANY 0x0000ffff

inline LONG RegGetValueW(HKEY /*hkey*/, LPCWSTR /*lpSubKey*/, LPCWSTR /*lpValue*/, DWORD /*dwFlags*/, LPDWORD /*pdwType*/, PVOID /*pvData*/, LPDWORD /*pcbData*/) {
    return ERROR_FILE_NOT_FOUND;
}

#define HCF_HIGHCONTRASTON 0x00000001
struct HIGHCONTRASTW {
    UINT cbSize;
    DWORD dwFlags;
    LPWSTR lpszDefaultScheme;
};

#define MONITOR_DEFAULTTONULL 0x00000000
#define MONITOR_DEFAULTTOPRIMARY 0x00000001
#define MONITOR_DEFAULTTONEAREST 0x00000002

#define IDC_ARROW MAKEINTRESOURCEW(32512)
#define IDC_IBEAM MAKEINTRESOURCEW(32513)
#define IDC_WAIT MAKEINTRESOURCEW(32514)
#define IDC_CROSS MAKEINTRESOURCEW(32515)
#define IDC_UPARROW MAKEINTRESOURCEW(32516)
#define IDC_SIZEALL MAKEINTRESOURCEW(32646)
#define IDC_SIZENWSE MAKEINTRESOURCEW(32642)
#define IDC_SIZENESW MAKEINTRESOURCEW(32643)
#define IDC_SIZEWE MAKEINTRESOURCEW(32644)
#define IDC_SIZENS MAKEINTRESOURCEW(32645)
#define IDC_NO MAKEINTRESOURCEW(32648)
#define IDC_HAND MAKEINTRESOURCEW(32649)
#define IDC_APPSTARTING MAKEINTRESOURCEW(32650)

#define IDI_APPLICATION MAKEINTRESOURCEW(32512)
#define IMAGE_BITMAP 0
#define IMAGE_ICON 1
#define IMAGE_CURSOR 2
#define LR_DEFAULTCOLOR 0x00000000
#define LR_SHARED 0x00008000
#define LR_DEFAULTSIZE 0x00000040
#define LR_LOADFROMFILE 0x00000010

#define CF_TEXT 1
#define CF_UNICODETEXT 13
#define CF_HDROP 15
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GHND (GMEM_MOVEABLE | GMEM_ZEROINIT)

#define TME_HOVER 0x00000001
#define TME_LEAVE 0x00000002
#define TME_NONCLIENT 0x00000010
#define TME_CANCEL 0x80000000

#define MF_STRING 0x00000000L
#define MF_SEPARATOR 0x00000800L
#define MF_ENABLED 0x00000000L
#define MF_GRAYED 0x00000001L
#define MF_DISABLED 0x00000002L
#define MF_CHECKED 0x00000008L
#define MF_POPUP 0x00000010L
#define TPM_LEFTALIGN 0x0000L
#define TPM_RIGHTALIGN 0x0008L
#define TPM_BOTTOMALIGN 0x0020L
#define TPM_RETURNCMD 0x0100L
#define TPM_NONOTIFY 0x0080L
#define TPM_RIGHTBUTTON 0x0002L

#define MB_OK 0x00000000L
#define MB_OKCANCEL 0x00000001L
#define MB_YESNOCANCEL 0x00000003L
#define MB_YESNO 0x00000004L
#define MB_ICONHAND 0x00000010L
#define MB_ICONQUESTION 0x00000020L
#define MB_ICONEXCLAMATION 0x00000030L
#define MB_ICONASTERISK 0x00000040L
#define MB_ICONERROR MB_ICONHAND
#define MB_ICONWARNING MB_ICONEXCLAMATION
#define MB_ICONINFORMATION MB_ICONASTERISK
#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7

#define COINIT_APARTMENTTHREADED 0x2
#define COINIT_MULTITHREADED 0x0
#define COINIT_DISABLE_OLE1DDE 0x4
#define CLSCTX_INPROC_SERVER 0x1
#define CLSCTX_LOCAL_SERVER 0x4
#define CLSCTX_ALL (CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER)

#define GA_PARENT 1
#define GA_ROOT 2
#define GA_ROOTOWNER 3

#define PM_NOREMOVE 0x0000
#define PM_REMOVE 0x0001
#define SMTO_NORMAL 0x0000
#define SMTO_BLOCK 0x0001
#define SMTO_ABORTIFHUNG 0x0002

#define ULW_ALPHA 0x00000002
#define AC_SRC_OVER 0x00
#define AC_SRC_ALPHA 0x01
#define DIB_RGB_COLORS 0
#define BI_RGB 0L
#define LOGPIXELSX 88
#define LOGPIXELSY 90

#define DWMWA_TRANSITIONS_FORCEDISABLED 3
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#define DWMWA_BORDER_COLOR 34
#define DWMWA_CAPTION_COLOR 35
#define DWMWA_TEXT_COLOR 36
#define DWMWCP_DEFAULT 0
#define DWMWCP_DONOTROUND 1
#define DWMWCP_ROUND 2
#define DWMWCP_ROUNDSMALL 3

#define NIM_ADD 0x00000000
#define NIM_MODIFY 0x00000001
#define NIM_DELETE 0x00000002
#define NIM_SETVERSION 0x00000004
#define NIF_MESSAGE 0x00000001
#define NIF_ICON 0x00000002
#define NIF_TIP 0x00000004
#define NIF_INFO 0x00000010
#define NOTIFYICON_VERSION_4 4

#define MOD_ALT 0x0001
#define MOD_CONTROL 0x0002
#define MOD_SHIFT 0x0004
#define MOD_WIN 0x0008
#define MOD_NOREPEAT 0x4000

// ============================================================================
// Win32 Structs
// ============================================================================

using WNDPROC = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
using TIMERPROC = void(CALLBACK*)(HWND, UINT, UINT_PTR, DWORD);
using MONITORENUMPROC = BOOL(CALLBACK*)(HMONITOR, HDC, LPRECT, LPARAM);

struct WNDCLASSEXW {
    UINT cbSize;
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
    HICON hIconSm;
};

struct CREATESTRUCTW {
    LPVOID lpCreateParams;
    HINSTANCE hInstance;
    HMENU hMenu;
    HWND hwndParent;
    int cy;
    int cx;
    int y;
    int x;
    LONG style;
    LPCWSTR lpszName;
    LPCWSTR lpszClass;
    DWORD dwExStyle;
};
using LPCREATESTRUCTW = CREATESTRUCTW*;

struct MSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
};
using LPMSG = MSG*;

struct MINMAXINFO {
    POINT ptReserved;
    POINT ptMaxSize;
    POINT ptMaxPosition;
    POINT ptMinTrackSize;
    POINT ptMaxTrackSize;
};

struct WINDOWPOS {
    HWND hwnd;
    HWND hwndInsertAfter;
    int x;
    int y;
    int cx;
    int cy;
    UINT flags;
};

struct NCCALCSIZE_PARAMS {
    RECT rgrc[3];
    WINDOWPOS* lppos;
};

struct WINDOWPLACEMENT {
    UINT length;
    UINT flags;
    UINT showCmd;
    POINT ptMinPosition;
    POINT ptMaxPosition;
    RECT rcNormalPosition;
};

struct MONITORINFO {
    DWORD cbSize;
    RECT rcMonitor;
    RECT rcWork;
    DWORD dwFlags;
};
using LPMONITORINFO = MONITORINFO*;

struct MONITORINFOEXW : public MONITORINFO {
    WCHAR szDevice[32];
};

struct DEVMODEW {
    WCHAR dmDeviceName[32];
    WORD dmSpecVersion;
    WORD dmDriverVersion;
    WORD dmSize;
    WORD dmDriverExtra;
    DWORD dmFields;
    DWORD dmBitsPerPel;
    DWORD dmPelsWidth;
    DWORD dmPelsHeight;
    DWORD dmDisplayFlags;
    DWORD dmDisplayFrequency;
};

inline BOOL EnumDisplaySettingsW(LPCWSTR /*lpszDeviceName*/, DWORD /*iModeNum*/, DEVMODEW* lpDevMode) {
    if (!lpDevMode) return FALSE;
    lpDevMode->dmDisplayFrequency = 60;
    lpDevMode->dmPelsWidth = 1920;
    lpDevMode->dmPelsHeight = 1080;
    lpDevMode->dmBitsPerPel = 32;
    return TRUE;
}

struct COPYDATASTRUCT {
    ULONG_PTR dwData;
    DWORD cbData;
    PVOID lpData;
};
using PCOPYDATASTRUCT = COPYDATASTRUCT*;

struct TRACKMOUSEEVENT {
    DWORD cbSize;
    DWORD dwFlags;
    HWND hwndTrack;
    DWORD dwHoverTime;
};
using LPTRACKMOUSEEVENT = TRACKMOUSEEVENT*;

struct PAINTSTRUCT {
    HDC hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    BYTE rgbReserved[32];
};

struct BITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth;
    LONG biHeight;
    WORD biPlanes;
    WORD biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG biXPelsPerMeter;
    LONG biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
};

struct RGBQUAD {
    BYTE rgbBlue;
    BYTE rgbGreen;
    BYTE rgbRed;
    BYTE rgbReserved;
};

struct BITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[1];
};

struct BLENDFUNCTION {
    BYTE BlendOp;
    BYTE BlendFlags;
    BYTE SourceConstantAlpha;
    BYTE AlphaFormat;
};

struct ICONINFO {
    BOOL fIcon;
    DWORD xHotspot;
    DWORD yHotspot;
    HBITMAP hbmMask;
    HBITMAP hbmColor;
};

struct MARGINS {
    int cxLeftWidth;
    int cxRightWidth;
    int cyTopHeight;
    int cyBottomHeight;
};

struct COMPOSITIONFORM {
    DWORD dwStyle;
    POINT ptCurrentPos;
    RECT rcArea;
};
#define CFS_POINT 0x0002
#define CFS_FORCE_POSITION 0x0020

struct TOUCHINPUT {
    LONG x;
    LONG y;
    HANDLE hSource;
    DWORD dwID;
    DWORD dwFlags;
    DWORD dwMask;
    DWORD dwTime;
    ULONG_PTR dwExtraInfo;
    DWORD cxContact;
    DWORD cyContact;
};
using PTOUCHINPUT = TOUCHINPUT*;
using HTOUCHINPUT = HANDLE;
#define TOUCHEVENTF_MOVE 0x0001
#define TOUCHEVENTF_DOWN 0x0002
#define TOUCHEVENTF_UP 0x0004
#define TOUCHEVENTF_PRIMARY 0x0010

struct WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    WCHAR cFileName[MAX_PATH];
    WCHAR cAlternateFileName[14];
};
using LPWIN32_FIND_DATAW = WIN32_FIND_DATAW*;

enum GET_FILEEX_INFO_LEVELS {
    GetFileExInfoStandard = 0,
    GetFileExMaxInfoLevel = 1,
};

struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
};

enum FINDEX_INFO_LEVELS {
    FindExInfoStandard = 0,
    FindExInfoBasic = 1,
};
enum FINDEX_SEARCH_OPS {
    FindExSearchNameMatch = 0,
    FindExSearchLimitToDirectories = 1,
};
#define FIND_FIRST_EX_LARGE_FETCH 0x00000002

struct BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD dwVolumeSerialNumber;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD nNumberOfLinks;
    DWORD nFileIndexHigh;
    DWORD nFileIndexLow;
};
using LPBY_HANDLE_FILE_INFORMATION = BY_HANDLE_FILE_INFORMATION*;

struct OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union {
        struct {
            DWORD Offset;
            DWORD OffsetHigh;
        };
        PVOID Pointer;
    };
    HANDLE hEvent;
};
using LPOVERLAPPED = OVERLAPPED*;

struct OPENFILENAMEW {
    DWORD lStructSize;
    HWND hwndOwner;
    HINSTANCE hInstance;
    LPCWSTR lpstrFilter;
    LPWSTR lpstrCustomFilter;
    DWORD nMaxCustFilter;
    DWORD nFilterIndex;
    LPWSTR lpstrFile;
    DWORD nMaxFile;
    LPWSTR lpstrFileTitle;
    DWORD nMaxFileTitle;
    LPCWSTR lpstrInitialDir;
    LPCWSTR lpstrTitle;
    DWORD Flags;
    WORD nFileOffset;
    WORD nFileExtension;
    LPCWSTR lpstrDefExt;
    LPARAM lCustData;
    void* lpfnHook;
    LPCWSTR lpTemplateName;
    void* pvReserved;
    DWORD dwReserved;
    DWORD FlagsEx;
};
using LPOPENFILENAMEW = OPENFILENAMEW*;

#define OFN_READONLY 0x00000001
#define OFN_OVERWRITEPROMPT 0x00000002
#define OFN_HIDEREADONLY 0x00000004
#define OFN_NOCHANGEDIR 0x00000008
#define OFN_ALLOWMULTISELECT 0x00000200
#define OFN_PATHMUSTEXIST 0x00000800
#define OFN_FILEMUSTEXIST 0x00001000
#define OFN_EXPLORER 0x00080000
#define OFN_ENABLESIZING 0x00800000

struct NOTIFYICONDATAW {
    DWORD cbSize;
    HWND hWnd;
    UINT uID;
    UINT uFlags;
    UINT uCallbackMessage;
    HICON hIcon;
    WCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    WCHAR szInfo[256];
    union {
        UINT uTimeout;
        UINT uVersion;
    };
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID guidItem;
    HICON hBalloonIcon;
};

struct SHELLEXECUTEINFOW {
    DWORD cbSize;
    ULONG fMask;
    HWND hwnd;
    LPCWSTR lpVerb;
    LPCWSTR lpFile;
    LPCWSTR lpParameters;
    LPCWSTR lpDirectory;
    int nShow;
    HINSTANCE hInstApp;
    void* lpIDList;
    LPCWSTR lpClass;
    HKEY hkeyClass;
    DWORD dwHotKey;
    union {
        HANDLE hIcon;
        HANDLE hMonitor;
    };
    HANDLE hProcess;
};
#define SEE_MASK_NOCLOSEPROCESS 0x00000040

struct PROCESSENTRY32W {
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ProcessID;
    ULONG_PTR th32DefaultHeapID;
    DWORD th32ModuleID;
    DWORD cntThreads;
    DWORD th32ParentProcessID;
    LONG pcPriClassBase;
    DWORD dwFlags;
    WCHAR szExeFile[MAX_PATH];
};
#define TH32CS_SNAPPROCESS 0x00000002

// ============================================================================
// Linux Platform Helper Declarations & Win32 API Function Declarations
// ============================================================================

std::string LinuxWideToUtf8(const wchar_t* wstr, int len = -1);
std::string LinuxWideToUtf8(const std::wstring& wstr);
std::wstring LinuxUtf8ToWide(const char* str, int len = -1);
std::wstring LinuxUtf8ToWide(const std::string& str);
std::string LinuxWinPathToPosix(const std::wstring& wpath);
std::wstring LinuxPosixPathToWin(const std::string& path);

DWORD GetLastError();
void SetLastError(DWORD dwErrCode);

int WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWSTR lpWideCharStr, int cchWideChar, LPSTR lpMultiByteStr, int cbMultiByte, LPCSTR lpDefaultChar, LPBOOL lpUsedDefaultChar);
int MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCSTR lpMultiByteStr, int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar);
int CompareStringOrdinal(LPCWSTR lpString1, int cchCount1, LPCWSTR lpString2, int cchCount2, BOOL bIgnoreCase);
int LCMapStringEx(LPCWSTR lpLocaleName, DWORD dwMapFlags, LPCWSTR lpSrcStr, int cchSrc, LPWSTR lpDestStr, int cchDest, void* lpVersionInformation, void* lpReserved, LPARAM sortHandle);
int GetLocaleInfoEx(LPCWSTR lpLocaleName, LCTYPE LCType, LPWSTR lpLCData, int cchData);
int GetUserDefaultLocaleName(LPWSTR lpLocaleName, int cchLocaleName);

BOOL PathIsRelativeW(LPCWSTR lpszPath);
BOOL PathRelativePathToW(LPWSTR pszPath, LPCWSTR pszFrom, DWORD dwAttrFrom, LPCWSTR pszTo, DWORD dwAttrTo);
HRESULT UrlCombineW(LPCWSTR pszBase, LPCWSTR pszRelative, LPWSTR pszCombined, LPDWORD pcchCombined, DWORD dwFlags);
LPWSTR* CommandLineToArgvW(LPCWSTR lpCmdLine, int* pNumArgs);
LPWSTR GetCommandLineW();
HLOCAL LocalFree(HLOCAL hMem);
void CoTaskMemFree(LPVOID pv);
LPVOID CoTaskMemAlloc(SIZE_T cb);

HRESULT CoInitializeEx(LPVOID pvReserved, DWORD dwCoInit);
void CoUninitialize();
HRESULT CoCreateInstance(REFCLSID rclsid, IUnknown* pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID* ppv);

DWORD GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize);
HMODULE GetModuleHandleW(LPCWSTR lpModuleName);
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004
#define LOAD_WITH_ALTERED_SEARCH_PATH 0x00000008
#define LOAD_LIBRARY_SEARCH_APPLICATION_DIR 0x00000200
#define LOAD_LIBRARY_SEARCH_USER_DIRS 0x00000400
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
using FARPROC = void(*)();
BOOL SetDllDirectoryW(LPCWSTR lpPathName);
BOOL SetDefaultDllDirectories(DWORD DirectoryFlags);
BOOL GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule);
HMODULE LoadLibraryW(LPCWSTR lpLibFileName);
HMODULE LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
FARPROC GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
BOOL FreeLibrary(HMODULE hLibModule);
DWORD GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR* lpFilePart);
DWORD GetTempPathW(DWORD nBufferLength, LPWSTR lpBuffer);
DWORD GetEnvironmentVariableW(LPCWSTR lpName, LPWSTR lpBuffer, DWORD nSize);
BOOL SetEnvironmentVariableW(LPCWSTR lpName, LPCWSTR lpValue);
DWORD ExpandEnvironmentStringsW(LPCWSTR lpSrc, LPWSTR lpDst, DWORD nSize);

DWORD GetFileAttributesW(LPCWSTR lpFileName);
BOOL GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation);
BOOL SetFileAttributesW(LPCWSTR lpFileName, DWORD dwFileAttributes);
HANDLE CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped);
BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped);
BOOL FlushFileBuffers(HANDLE hFile);
DWORD SetFilePointer(HANDLE hFile, LONG lDistanceToMove, LONG* lpDistanceToMoveHigh, DWORD dwMoveMethod);
BOOL SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER lpNewFilePointer, DWORD dwMoveMethod);
BOOL SetEndOfFile(HANDLE hFile);
BOOL GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize);
BOOL GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation);
BOOL SetFileTime(HANDLE hFile, const FILETIME* lpCreationTime, const FILETIME* lpLastAccessTime, const FILETIME* lpLastWriteTime);
BOOL CloseHandle(HANDLE hObject);
BOOL DeleteFileW(LPCWSTR lpFileName);
BOOL MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags);
BOOL CopyFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, BOOL bFailIfExists);
BOOL CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
BOOL RemoveDirectoryW(LPCWSTR lpPathName);
int SHCreateDirectoryExW(HWND hwnd, LPCWSTR pszPath, const void* psa);
HANDLE FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData);
HANDLE FindFirstFileExW(LPCWSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId, LPVOID lpFindFileData, FINDEX_SEARCH_OPS fSearchOp, LPVOID lpSearchFilter, DWORD dwAdditionalFlags);
BOOL FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData);
BOOL FindClose(HANDLE hFindFile);
BOOL GetDiskFreeSpaceExW(LPCWSTR lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller, PULARGE_INTEGER lpTotalNumberOfBytes, PULARGE_INTEGER lpTotalNumberOfFreeBytes);
BOOL CancelSynchronousIo(HANDLE hThread);
inline BOOL CancelSynchronousIo(unsigned long /*hThread*/) { return TRUE; }
BOOL CancelIoEx(HANDLE hFile, LPOVERLAPPED lpOverlapped);

ULONGLONG GetTickCount64();
DWORD GetTickCount();
BOOL QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency);
void GetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime);
void GetSystemTime(LPSYSTEMTIME lpSystemTime);
void GetLocalTime(LPSYSTEMTIME lpSystemTime);
BOOL FileTimeToSystemTime(const FILETIME* lpFileTime, LPSYSTEMTIME lpSystemTime);
BOOL SystemTimeToFileTime(const SYSTEMTIME* lpSystemTime, LPFILETIME lpFileTime);
BOOL SystemTimeToTzSpecificLocalTime(const void* lpTimeZoneInformation, const SYSTEMTIME* lpUniversalTime, LPSYSTEMTIME lpLocalTime);
void Sleep(DWORD dwMilliseconds);

DWORD GetCurrentProcessId();
DWORD GetCurrentThreadId();
HANDLE GetCurrentProcess();
HANDLE GetCurrentThread();
BOOL SetThreadPriority(HANDLE hThread, int nPriority);
EXECUTION_STATE SetThreadExecutionState(EXECUTION_STATE esFlags);
BOOL ProcessIdToSessionId(DWORD dwProcessId, DWORD* pSessionId);
HANDLE OpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId);
BOOL TerminateProcess(HANDLE hProcess, UINT uExitCode);
BOOL GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode);
HANDLE CreateToolhelp32Snapshot(DWORD dwFlags, DWORD th32ProcessID);
BOOL Process32FirstW(HANDLE hSnapshot, PROCESSENTRY32W* lppe);
BOOL Process32NextW(HANDLE hSnapshot, PROCESSENTRY32W* lppe);

HANDLE CreateEventW(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState, LPCWSTR lpName);
BOOL SetEvent(HANDLE hEvent);
BOOL ResetEvent(HANDLE hEvent);
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCWSTR lpName);
BOOL ReleaseMutex(HANDLE hMutex);
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES lpTimerAttributes, LPCWSTR lpTimerName, DWORD dwFlags, DWORD dwDesiredAccess);
BOOL SetWaitableTimer(HANDLE hTimer, const LARGE_INTEGER* lpDueTime, LONG lPeriod, void* pfnCompletionRoutine, LPVOID lpArgToCompletionRoutine, BOOL fResume);
BOOL CancelWaitableTimer(HANDLE hTimer);
DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
DWORD WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds);
DWORD MsgWaitForMultipleObjects(DWORD nCount, const HANDLE* pHandles, BOOL fWaitAll, DWORD dwMilliseconds, DWORD dwWakeMask);

ATOM RegisterClassExW(const WNDCLASSEXW* lpwcx);
HWND CreateWindowExW(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam);
BOOL DestroyWindow(HWND hWnd);
BOOL IsWindow(HWND hWnd);
BOOL IsWindowVisible(HWND hWnd);
BOOL IsIconic(HWND hWnd);
BOOL IsZoomed(HWND hWnd);
BOOL ShowWindow(HWND hWnd, int nCmdShow);
BOOL SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags);
BOOL MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint);
BOOL GetClientRect(HWND hWnd, LPRECT lpRect);
BOOL GetWindowRect(HWND hWnd, LPRECT lpRect);
BOOL ClientToScreen(HWND hWnd, LPPOINT lpPoint);
BOOL ScreenToClient(HWND hWnd, LPPOINT lpPoint);
int MapWindowPoints(HWND hWndFrom, HWND hWndTo, LPPOINT lpPoints, UINT cPoints);
HWND GetParent(HWND hWnd);
HWND SetParent(HWND hWndChild, HWND hWndNewParent);
HWND GetAncestor(HWND hWnd, UINT gaFlags);
BOOL SetForegroundWindow(HWND hWnd);
HWND GetForegroundWindow();
HWND SetFocus(HWND hWnd);
HWND GetFocus();
BOOL SetWindowTextW(HWND hWnd, LPCWSTR lpString);
int GetWindowTextW(HWND hWnd, LPWSTR lpString, int nMaxCount);
LONG_PTR GetWindowLongPtrW(HWND hWnd, int nIndex);
LONG_PTR SetWindowLongPtrW(HWND hWnd, int nIndex, LONG_PTR dwNewLong);
inline LONG GetWindowLongW(HWND hWnd, int nIndex) { return static_cast<LONG>(GetWindowLongPtrW(hWnd, nIndex)); }
inline LONG SetWindowLongW(HWND hWnd, int nIndex, LONG dwNewLong) { return static_cast<LONG>(SetWindowLongPtrW(hWnd, nIndex, dwNewLong)); }
#define GetWindowLong GetWindowLongW
#define SetWindowLong SetWindowLongW
#define GetWindowLongPtr GetWindowLongPtrW
#define SetWindowLongPtr SetWindowLongPtrW
BOOL GetWindowPlacement(HWND hWnd, WINDOWPLACEMENT* lpwndpl);
BOOL SetWindowPlacement(HWND hWnd, const WINDOWPLACEMENT* lpwndpl);
BOOL AdjustWindowRectEx(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle);
BOOL AdjustWindowRectExForDpi(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle, UINT dpi);

LRESULT DefWindowProcW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
BOOL PostMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
LRESULT SendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
LRESULT SendMessageTimeoutW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, UINT fuFlags, UINT uTimeout, DWORD_PTR* lpdwResult);
void PostQuitMessage(int nExitCode);
UINT RegisterWindowMessageW(LPCWSTR lpString);
BOOL GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
BOOL PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
BOOL TranslateMessage(const MSG* lpMsg);
LRESULT DispatchMessageW(const MSG* lpMsg);

UINT_PTR SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc);
BOOL KillTimer(HWND hWnd, UINT_PTR uIDEvent);
BOOL InvalidateRect(HWND hWnd, const RECT* lpRect, BOOL bErase);
BOOL UpdateWindow(HWND hWnd);

SHORT GetKeyState(int nVirtKey);
SHORT GetAsyncKeyState(int nVirtKey);
BOOL GetCursorPos(LPPOINT lpPoint);
HCURSOR LoadCursorW(HINSTANCE hInstance, LPCWSTR lpCursorName);
HCURSOR SetCursor(HCURSOR hCursor);
int ShowCursor(BOOL bShow);
HWND SetCapture(HWND hWnd);
BOOL ReleaseCapture();
HWND GetCapture();
BOOL TrackMouseEvent(LPTRACKMOUSEEVENT lpEventTrack);
UINT GetDoubleClickTime();

HMONITOR MonitorFromWindow(HWND hwnd, DWORD dwFlags);
HMONITOR MonitorFromPoint(POINT pt, DWORD dwFlags);
HMONITOR MonitorFromRect(LPCRECT lprc, DWORD dwFlags);
BOOL GetMonitorInfoW(HMONITOR hMonitor, LPMONITORINFO lpmi);
#define GetMonitorInfo GetMonitorInfoW
inline int MulDiv(int nNumber, int nNumerator, int nDenominator) {
    if (nDenominator == 0) return -1;
    int64_t prod = static_cast<int64_t>(nNumber) * static_cast<int64_t>(nNumerator);
    if ((prod >= 0 && nDenominator > 0) || (prod < 0 && nDenominator < 0)) {
        prod += nDenominator / 2;
    } else {
        prod -= nDenominator / 2;
    }
    return static_cast<int>(prod / nDenominator);
}
BOOL EnumDisplayMonitors(HDC hdc, LPCRECT lprcClip, MONITORENUMPROC lpfnEnum, LPARAM dwData);
UINT GetDpiForWindow(HWND hwnd);
UINT GetDpiForSystem();
int GetSystemMetrics(int nIndex);
int GetSystemMetricsForDpi(int nIndex, UINT dpi);
BOOL SystemParametersInfoW(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni);
DWORD GetSysColor(int nIndex);

BOOL SetRect(LPRECT lprc, int xLeft, int yTop, int xRight, int yBottom);
BOOL SetRectEmpty(LPRECT lprc);
BOOL IsRectEmpty(const RECT* lprc);
BOOL EqualRect(const RECT* lprc1, const RECT* lprc2);
BOOL PtInRect(const RECT* lprc, POINT pt);
BOOL IntersectRect(LPRECT lprcDst, const RECT* lprcSrc1, const RECT* lprcSrc2);
BOOL UnionRect(LPRECT lprcDst, const RECT* lprcSrc1, const RECT* lprcSrc2);
BOOL OffsetRect(LPRECT lprc, int dx, int dy);
BOOL InflateRect(LPRECT lprc, int dx, int dy);

HDC GetDC(HWND hWnd);
int ReleaseDC(HWND hWnd, HDC hDC);
int GetDeviceCaps(HDC hdc, int index);
HDC CreateCompatibleDC(HDC hdc);
BOOL DeleteDC(HDC hdc);
HBITMAP CreateDIBSection(HDC hdc, const BITMAPINFO* pbmi, UINT usage, void** ppvBits, HANDLE hSection, DWORD offset);
HBITMAP CreateBitmap(int nWidth, int nHeight, UINT nPlanes, UINT nBitCount, const void* lpBits);
HGDIOBJ SelectObject(HDC hdc, HGDIOBJ h);
BOOL DeleteObject(HGDIOBJ ho);
HBRUSH CreateSolidBrush(COLORREF color);
int FillRect(HDC hDC, const RECT* lprc, HBRUSH hbr);
HDC BeginPaint(HWND hWnd, PAINTSTRUCT* lpPaint);
BOOL EndPaint(HWND hWnd, const PAINTSTRUCT* lpPaint);
BOOL UpdateLayeredWindow(HWND hWnd, HDC hdcDst, POINT* pptDst, SIZE* psize, HDC hdcSrc, POINT* pptSrc, COLORREF crKey, BLENDFUNCTION* pblend, DWORD dwFlags);

HRESULT DwmSetWindowAttribute(HWND hwnd, DWORD dwAttribute, LPCVOID pvAttribute, DWORD cbAttribute);
inline HRESULT DwmGetWindowAttribute(HWND hwnd, DWORD dwAttribute, PVOID pvAttribute, DWORD cbAttribute) {
    if (!pvAttribute) return E_POINTER;
    if (dwAttribute == DWMWA_EXTENDED_FRAME_BOUNDS && cbAttribute >= sizeof(RECT)) {
        GetWindowRect(hwnd, static_cast<LPRECT>(pvAttribute));
        return S_OK;
    }
    std::memset(pvAttribute, 0, cbAttribute);
    return S_OK;
}
HRESULT DwmExtendFrameIntoClientArea(HWND hWnd, const MARGINS* pMarInset);
HRESULT DwmFlush();

BOOL OpenClipboard(HWND hWndNewOwner);
BOOL CloseClipboard();
BOOL EmptyClipboard();
HANDLE GetClipboardData(UINT uFormat);
HANDLE SetClipboardData(UINT uFormat, HANDLE hMem);
BOOL IsClipboardFormatAvailable(UINT format);
HGLOBAL GlobalAlloc(UINT uFlags, SIZE_T dwBytes);
LPVOID GlobalLock(HGLOBAL hMem);
BOOL GlobalUnlock(HGLOBAL hMem);
SIZE_T GlobalSize(HGLOBAL hMem);
HGLOBAL GlobalFree(HGLOBAL hMem);

UINT DragQueryFileW(HDROP hDrop, UINT iFile, LPWSTR lpszFile, UINT cch);
void DragFinish(HDROP hDrop);
void DragAcceptFiles(HWND hWnd, BOOL fAccept);

HINSTANCE ShellExecuteW(HWND hwnd, LPCWSTR lpOperation, LPCWSTR lpFile, LPCWSTR lpParameters, LPCWSTR lpDirectory, int nShowCmd);
BOOL ShellExecuteExW(SHELLEXECUTEINFOW* pExecInfo);
BOOL Shell_NotifyIconW(DWORD dwMessage, NOTIFYICONDATAW* lpData);
BOOL RegisterHotKey(HWND hWnd, int id, UINT fsModifiers, UINT vk);
BOOL UnregisterHotKey(HWND hWnd, int id);

HANDLE LoadImageW(HINSTANCE hInst, LPCWSTR name, UINT type, int cx, int cy, UINT fuLoad);
HICON CreateIconIndirect(ICONINFO* piconinfo);
BOOL DestroyIcon(HICON hIcon);
HMENU CreatePopupMenu();
BOOL AppendMenuW(HMENU hMenu, UINT uFlags, UINT_PTR uIDNewItem, LPCWSTR lpNewItem);
BOOL TrackPopupMenu(HMENU hMenu, UINT uFlags, int x, int y, int nReserved, HWND hWnd, const RECT* prcRect);
BOOL DestroyMenu(HMENU hMenu);
int MessageBoxW(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType);

BOOL GetOpenFileNameW(LPOPENFILENAMEW lpofn);
BOOL GetSaveFileNameW(LPOPENFILENAMEW lpofn);

HIMC ImmGetContext(HWND hWnd);
BOOL ImmSetCompositionWindow(HIMC hIMC, COMPOSITIONFORM* lpCompForm);
BOOL ImmReleaseContext(HWND hWnd, HIMC hIMC);
#define GCS_COMPSTR 0x0008
#define GCS_RESULTSTR 0x0800
inline LONG ImmGetCompositionStringW(HIMC /*hIMC*/, DWORD /*dwIndex*/, LPVOID /*lpBuf*/, DWORD /*dwBufLen*/) { return 0; }

BOOL RegisterTouchWindow(HWND hwnd, ULONG ulFlags);
BOOL UnregisterTouchWindow(HWND hwnd);
BOOL GetTouchInputInfo(HTOUCHINPUT hTouchInput, UINT cInputs, PTOUCHINPUT pInputs, int cbSize);
BOOL CloseTouchInputHandle(HTOUCHINPUT hTouchInput);

void OutputDebugStringW(LPCWSTR lpOutputString);
void OutputDebugStringA(LPCSTR lpOutputString);

// ============================================================================
// Common Controls / Tooltip / Theme / OLE Drag & Drop / Process Tokens
// ============================================================================

#define ICC_WIN95_CLASSES 0x000000FF
#define TOOLTIPS_CLASSW L"tooltips_class32"
#define TTS_ALWAYSTIP 0x01
#define TTS_NOPREFIX 0x02
#define TTF_IDISHWND 0x0001
#define TTF_CENTERTIP 0x0002
#define TTF_RTLREADING 0x0004
#define TTF_SUBCLASS 0x0010
#define TTF_TRACK 0x0020
#define TTF_ABSOLUTE 0x0080
#define TTF_TRANSPARENT 0x0100
#define TTF_PARSELINKS 0x1000
#define TTF_DI_SETITEM 0x8000

#define TTDT_AUTOMATIC 0
#define TTDT_RESHOW 1
#define TTDT_AUTOPOP 2
#define TTDT_INITIAL 3

#define TTM_ACTIVATE (WM_USER + 1)
#define TTM_SETDELAYTIME (WM_USER + 3)
#define TTM_RELAYEVENT (WM_USER + 7)
#define TTM_TRACKACTIVATE (WM_USER + 17)
#define TTM_TRACKPOSITION (WM_USER + 18)
#define TTM_SETTIPBKCOLOR (WM_USER + 19)
#define TTM_SETTIPTEXTCOLOR (WM_USER + 20)
#define TTM_SETMAXTIPWIDTH (WM_USER + 24)
#define TTM_POP (WM_USER + 28)
#define TTM_ADDTOOLW (WM_USER + 50)
#define TTM_DELTOOLW (WM_USER + 51)
#define TTM_NEWTOOLRECTW (WM_USER + 52)
#define TTM_HITTESTW (WM_USER + 55)
#define TTM_HITTEST TTM_HITTESTW
#define TTM_UPDATETIPTEXTW (WM_USER + 57)

#define COLOR_WINDOW 5
#define COLOR_WINDOWTEXT 8
#define COLOR_HIGHLIGHT 13
#define COLOR_HIGHLIGHTTEXT 14
#define COLOR_3DSHADOW 16
#define COLOR_GRAYTEXT 17
#define COLOR_INFOTEXT 23
#define COLOR_INFOBK 24

struct INITCOMMONCONTROLSEX {
    DWORD dwSize;
    DWORD dwICC;
};

struct TOOLINFOW {
    UINT cbSize;
    UINT uFlags;
    HWND hwnd;
    UINT_PTR uId;
    RECT rect;
    HINSTANCE hinst;
    LPWSTR lpszText;
    LPARAM lParam;
    void* lpReserved;
};

struct TTHITTESTINFOW {
    HWND hwnd;
    POINT pt;
    TOOLINFOW ti;
};
using TTHITTESTINFO = TTHITTESTINFOW;

inline BOOL InitCommonControlsEx(const INITCOMMONCONTROLSEX* /*picce*/) { return TRUE; }
inline HRESULT SetWindowTheme(HWND /*hwnd*/, LPCWSTR /*pszSubAppName*/, LPCWSTR /*pszSubIdList*/) { return S_OK; }
inline LONG GetMessageTime() { return static_cast<LONG>(GetTickCount()); }
inline DWORD GetWindowThreadProcessId(HWND /*hWnd*/, LPDWORD lpdwProcessId) {
    if (lpdwProcessId) *lpdwProcessId = GetCurrentProcessId();
    return GetCurrentThreadId();
}
inline BOOL DragQueryPoint(HDROP /*hDrop*/, LPPOINT lppt) {
    if (lppt) { lppt->x = 0; lppt->y = 0; }
    return TRUE;
}

// OLE Clipboard & Drag-and-Drop
using CLIPFORMAT = WORD;
#define DVASPECT_CONTENT 1
#define TYMED_HGLOBAL 1
#define DROPEFFECT_NONE 0
#define DROPEFFECT_COPY 1
#define DROPEFFECT_MOVE 2
#define DROPEFFECT_LINK 4

struct FORMATETC {
    CLIPFORMAT cfFormat;
    void* ptd;
    DWORD dwAspect;
    LONG lindex;
    DWORD tymed;
};

struct STGMEDIUM {
    DWORD tymed;
    union {
        HBITMAP hBitmap;
        HGLOBAL hGlobal;
        LPOLESTR lpszFileName;
        IStream* pstm;
    };
    IUnknown* pUnkForRelease;
};

struct IDataObject : public IUnknown {
    virtual HRESULT GetData(FORMATETC* /*pformatetcIn*/, STGMEDIUM* /*pmedium*/) { return E_FAIL; }
    virtual HRESULT QueryGetData(FORMATETC* /*pformatetc*/) { return E_FAIL; }
};

struct IDropTarget : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) = 0;
    virtual HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) = 0;
    virtual HRESULT STDMETHODCALLTYPE DragLeave() = 0;
    virtual HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) = 0;
};

inline HRESULT OleInitialize(LPVOID /*pvReserved*/) { return S_OK; }
inline void OleUninitialize() {}
inline HRESULT RegisterDragDrop(HWND /*hwnd*/, IDropTarget* /*pDropTarget*/) { return S_OK; }
inline HRESULT RevokeDragDrop(HWND /*hwnd*/) { return S_OK; }
inline void ReleaseStgMedium(STGMEDIUM* pmedium) {
    if (!pmedium) return;
    if (pmedium->pUnkForRelease) {
        pmedium->pUnkForRelease->Release();
        pmedium->pUnkForRelease = nullptr;
    } else if (pmedium->tymed == TYMED_HGLOBAL && pmedium->hGlobal) {
        GlobalFree(pmedium->hGlobal);
        pmedium->hGlobal = nullptr;
    }
}
inline UINT RegisterClipboardFormatW(LPCWSTR lpszFormat) {
    if (!lpszFormat) return 0;
    uint32_t h = 2166136261u;
    for (const wchar_t* p = lpszFormat; *p; ++p) h = (h ^ static_cast<uint32_t>(*p)) * 16777619u;
    return 0xC000u + (h & 0x3FFFu);
}

// Process & Security Token helpers
#define CREATE_NO_WINDOW 0x08000000
#define DETACHED_PROCESS 0x00000008

struct STARTUPINFOW {
    DWORD cb;
    LPWSTR lpReserved;
    LPWSTR lpDesktop;
    LPWSTR lpTitle;
    DWORD dwX;
    DWORD dwY;
    DWORD dwXSize;
    DWORD dwYSize;
    DWORD dwXCountChars;
    DWORD dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
};

struct PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
};

inline BOOL CreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine, LPSECURITY_ATTRIBUTES /*lpProcessAttributes*/,
                           LPSECURITY_ATTRIBUTES /*lpThreadAttributes*/, BOOL /*bInheritHandles*/, DWORD /*dwCreationFlags*/,
                           LPVOID /*lpEnvironment*/, LPCWSTR lpCurrentDirectory, STARTUPINFOW* /*lpStartupInfo*/,
                           PROCESS_INFORMATION* lpProcessInformation) {
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    std::wstring fileStr;
    std::wstring paramStr;
    if (lpApplicationName && *lpApplicationName) {
        fileStr = lpApplicationName;
        if (lpCommandLine) paramStr = lpCommandLine;
    } else if (lpCommandLine && *lpCommandLine) {
        std::wstring cmd = lpCommandLine;
        if (cmd.front() == L'"') {
            size_t endQ = cmd.find(L'"', 1);
            if (endQ != std::wstring::npos) {
                fileStr = cmd.substr(1, endQ - 1);
                if (endQ + 1 < cmd.size()) paramStr = cmd.substr(endQ + 1);
            } else {
                fileStr = cmd.substr(1);
            }
        } else {
            size_t sp = cmd.find(L' ');
            if (sp != std::wstring::npos) {
                fileStr = cmd.substr(0, sp);
                paramStr = cmd.substr(sp + 1);
            } else {
                fileStr = cmd;
            }
        }
    }
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = fileStr.c_str();
    sei.lpParameters = paramStr.empty() ? nullptr : paramStr.c_str();
    sei.lpDirectory = lpCurrentDirectory;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return FALSE;
    if (lpProcessInformation) {
        lpProcessInformation->hProcess = sei.hProcess;
        lpProcessInformation->hThread = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x2001));
        lpProcessInformation->dwProcessId = 0;
        lpProcessInformation->dwThreadId = 0;
    }
    return TRUE;
}

#define DUPLICATE_SAME_ACCESS 0x00000002
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#define TOKEN_QUERY 0x0008
#define SECURITY_MAX_SID_SIZE 68

using PSID = void*;

enum TOKEN_INFORMATION_CLASS {
    TokenUser = 1,
    TokenSessionId = 12,
};

struct SID_AND_ATTRIBUTES {
    PSID Sid;
    DWORD Attributes;
};

struct TOKEN_USER {
    SID_AND_ATTRIBUTES User;
};
using PTOKEN_USER = TOKEN_USER*;

inline BOOL DuplicateHandle(HANDLE /*hSourceProcessHandle*/, HANDLE hSourceHandle, HANDLE /*hTargetProcessHandle*/, LPHANDLE lpTargetHandle, DWORD /*dwDesiredAccess*/, BOOL /*bInheritHandle*/, DWORD /*dwOptions*/) {
    if (lpTargetHandle) *lpTargetHandle = hSourceHandle;
    return TRUE;
}
inline BOOL OpenProcessToken(HANDLE /*ProcessHandle*/, DWORD /*DesiredAccess*/, PHANDLE TokenHandle) {
    if (!TokenHandle) return FALSE;
    *TokenHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x7001));
    return TRUE;
}
inline BOOL GetTokenInformation(HANDLE /*TokenHandle*/, TOKEN_INFORMATION_CLASS TokenInformationClass, LPVOID TokenInformation, DWORD TokenInformationLength, PDWORD ReturnLength) {
    if (TokenInformationClass == TokenSessionId) {
        if (ReturnLength) *ReturnLength = sizeof(DWORD);
        if (!TokenInformation || TokenInformationLength < sizeof(DWORD)) return FALSE;
        *static_cast<DWORD*>(TokenInformation) = 1;
        return TRUE;
    }
    if (TokenInformationClass == TokenUser) {
        if (ReturnLength) *ReturnLength = sizeof(TOKEN_USER) + sizeof(uint32_t);
        if (!TokenInformation || TokenInformationLength < sizeof(TOKEN_USER) + sizeof(uint32_t)) return FALSE;
        auto* tu = static_cast<PTOKEN_USER>(TokenInformation);
        auto* sid = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(TokenInformation) + sizeof(TOKEN_USER));
        *sid = 1000u;
        tu->User.Sid = sid;
        tu->User.Attributes = 0;
        return TRUE;
    }
    return FALSE;
}
inline BOOL IsValidSid(PSID pSid) { return pSid != nullptr; }
inline BOOL EqualSid(PSID pSid1, PSID pSid2) {
    if (!pSid1 || !pSid2) return FALSE;
    return *static_cast<const uint32_t*>(pSid1) == *static_cast<const uint32_t*>(pSid2);
}

// Message-only window & UnregisterClassW
#define HWND_MESSAGE ((HWND)(intptr_t)-3)
inline BOOL UnregisterClassW(LPCWSTR /*lpClassName*/, HINSTANCE /*hInstance*/) { return TRUE; }

// Pointer & Tablet Gesture Constants/Structs
#define WM_POINTERUPDATE 0x0245
#define WM_POINTERDOWN 0x0246
#define WM_POINTERUP 0x0247
#define WM_POINTERENTER 0x0249
#define WM_POINTERLEAVE 0x024A
#define WM_POINTERACTIVATE 0x024B
#define WM_POINTERCAPTURECHANGED 0x024C
#define WM_TABLET_QUERYSYSTEMGESTURESTATUS 0x02CC

#define GET_POINTERID_WPARAM(wParam) (LOWORD(wParam))

using POINTER_INPUT_TYPE = DWORD;
#define PT_POINTER 1
#define PT_TOUCH 2
#define PT_PEN 3
#define PT_MOUSE 4
#define PT_TOUCHPAD 5

#define POINTER_FLAG_NONE 0x00000000
#define POINTER_FLAG_NEW 0x00000001
#define POINTER_FLAG_INRANGE 0x00000002
#define POINTER_FLAG_INCONTACT 0x00000004
#define POINTER_FLAG_FIRSTBUTTON 0x00000010
#define POINTER_FLAG_SECONDBUTTON 0x00000020
#define POINTER_FLAG_THIRDBUTTON 0x00000040
#define POINTER_FLAG_FOURTHBUTTON 0x00000080
#define POINTER_FLAG_FIFTHBUTTON 0x00000100
#define POINTER_FLAG_PRIMARY 0x00002000
#define POINTER_FLAG_CONFIDENCE 0x00004000
#define POINTER_FLAG_CANCELED 0x00008000
#define POINTER_FLAG_DOWN 0x00010000
#define POINTER_FLAG_UPDATE 0x00020000
#define POINTER_FLAG_UP 0x00040000

struct POINTER_INFO {
    POINTER_INPUT_TYPE pointerType;
    UINT32 pointerId;
    UINT32 frameId;
    UINT32 pointerFlags;
    HANDLE sourceDevice;
    HWND hwndTarget;
    POINT ptPixelLocation;
    POINT ptHimetricLocation;
    POINT ptPixelLocationRaw;
    POINT ptHimetricLocationRaw;
    DWORD dwTime;
    UINT32 historyCount;
    INT32 InputData;
    DWORD dwKeyStates;
    UINT64 PerformanceCount;
    int ButtonChangeType;
};

inline BOOL GetPointerType(UINT32 /*pointerId*/, POINTER_INPUT_TYPE* pointerType) {
    if (pointerType) *pointerType = PT_MOUSE;
    return FALSE;
}
inline BOOL GetPointerInfo(UINT32 /*pointerId*/, POINTER_INFO* /*pointerInfo*/) { return FALSE; }

#define TABLET_DISABLE_PRESSANDHOLD 0x00000001
#define TABLET_DISABLE_PENTAPFEEDBACK 0x00000008
#define TABLET_DISABLE_PENBARRELFEEDBACK 0x00000010
#define TABLET_DISABLE_TOUCHUIFORCEON 0x00000100
#define TABLET_DISABLE_TOUCHUIFORCEOFF 0x00000200
#define TABLET_DISABLE_TOUCHSWITCH 0x00008000
#define TABLET_DISABLE_FLICKS 0x00010000
#define TABLET_ENABLE_FLICKSONCONTEXT 0x00020000
#define TABLET_ENABLE_FLICKLEARNINGMODE 0x00040000
#define TABLET_DISABLE_SMOOTHSCROLLING 0x00080000
#define TABLET_DISABLE_FLICKFALLBACKKEYS 0x00100000
#define TABLET_ENABLE_MULTITOUCHDATA 0x01000000

#define GC_ALLGESTURES 0x00000001
struct GESTURECONFIG {
    DWORD dwID;
    DWORD dwWant;
    DWORD dwBlock;
};
inline BOOL SetGestureConfig(HWND /*hwnd*/, DWORD /*dwReserved*/, UINT /*cIDs*/, GESTURECONFIG* /*pGestureConfig*/, UINT /*cbSize*/) { return TRUE; }

#ifndef ERROR_REVISION_MISMATCH
#define ERROR_REVISION_MISMATCH 1306L
#endif

inline LONG CompareFileTime(const FILETIME* lpFileTime1, const FILETIME* lpFileTime2) {
    if (!lpFileTime1 || !lpFileTime2) return 0;
    uint64_t a = (static_cast<uint64_t>(lpFileTime1->dwHighDateTime) << 32) | lpFileTime1->dwLowDateTime;
    uint64_t b = (static_cast<uint64_t>(lpFileTime2->dwHighDateTime) << 32) | lpFileTime2->dwLowDateTime;
    return (a < b) ? -1 : ((a > b) ? 1 : 0);
}

inline HRESULT CoCreateGuid(GUID* pguid) {
    if (!pguid) return E_POINTER;
    static std::atomic<uint64_t> s_counter{0x9E3779B97F4A7C15ULL};
    uint64_t t = static_cast<uint64_t>(GetTickCount64());
    uint64_t c = s_counter.fetch_add(0xBF58476D1CE4E5B9ULL, std::memory_order_relaxed);
    uint64_t h1 = (t ^ c) * 0x94D049BB133111EBULL;
    uint64_t h2 = (c + 0x100000001B3ULL) * 0xBF58476D1CE4E5B9ULL;
    pguid->Data1 = static_cast<uint32_t>(h1 >> 32);
    pguid->Data2 = static_cast<uint16_t>((h1 >> 16) & 0xFFFF);
    pguid->Data3 = static_cast<uint16_t>((h1 & 0x0FFF) | 0x4000);
    for (int i = 0; i < 8; ++i) pguid->Data4[i] = static_cast<uint8_t>((h2 >> (i * 8)) & 0xFF);
    pguid->Data4[0] = static_cast<uint8_t>((pguid->Data4[0] & 0x3F) | 0x80);
    return S_OK;
}

inline int StringFromGUID2(REFGUID rguid, LPOLESTR lpsz, int cchMax) {
    if (!lpsz || cchMax < 39) return 0;
    int written = std::swprintf(
        lpsz, static_cast<size_t>(cchMax),
        L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        rguid.Data1, rguid.Data2, rguid.Data3,
        rguid.Data4[0], rguid.Data4[1], rguid.Data4[2], rguid.Data4[3],
        rguid.Data4[4], rguid.Data4[5], rguid.Data4[6], rguid.Data4[7]);
    return (written > 0) ? (written + 1) : 0;
}

inline constexpr GUID IID_IDropTarget = __uuidof(IDropTarget);
inline constexpr GUID IID_IDataObject = __uuidof(IDataObject);

#ifndef DefWindowProc
#define DefWindowProc DefWindowProcW
#endif
#ifndef LoadImage
#define LoadImage LoadImageW
#endif
#ifndef LoadCursor
#define LoadCursor LoadCursorW
#endif
#ifndef SendMessage
#define SendMessage SendMessageW
#endif
#ifndef PostMessage
#define PostMessage PostMessageW
#endif
#ifndef FindWindow
#define FindWindow FindWindowW
#endif
#ifndef FindWindowEx
#define FindWindowEx FindWindowExW
#endif
#ifndef GetClassName
#define GetClassName GetClassNameW
#endif
#ifndef UNICODE_NOCHAR
#define UNICODE_NOCHAR 0xFFFF
#endif
#ifndef SW_FORCEMINIMIZE
#define SW_FORCEMINIMIZE 11
#endif
#ifndef WM_CANCELMODE
#define WM_CANCELMODE 0x001F
#endif
#ifndef SM_CXDRAG
#define SM_CXDRAG 68
#endif
#ifndef SM_CYDRAG
#define SM_CYDRAG 69
#endif

// IME Candidate window support
#ifndef GCS_CURSORPOS
#define GCS_CURSORPOS 0x0080
#endif
#ifndef CFS_CANDIDATEPOS
#define CFS_CANDIDATEPOS 0x0040
#endif
struct CANDIDATEFORM {
    DWORD dwIndex;
    DWORD dwStyle;
    POINT ptCurrentPos;
    RECT rcArea;
};
using LPCANDIDATEFORM = CANDIDATEFORM*;
inline BOOL ImmSetCandidateWindow(HIMC /*himc*/, LPCANDIDATEFORM /*lpCandidate*/) { return TRUE; }

inline UINT GetSystemDirectoryW(LPWSTR lpBuffer, UINT uSize) {
    if (lpBuffer && uSize > 0) lpBuffer[0] = L'\0';
    return 0;
}

// Process Power Throttling (EcoQoS)
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
struct PROCESS_POWER_THROTTLING_STATE {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};
enum PROCESS_INFORMATION_CLASS {
    ProcessMemoryPriority = 0,
    ProcessPowerThrottling = 4
};
inline BOOL SetProcessInformation(HANDLE /*hProcess*/, PROCESS_INFORMATION_CLASS /*ProcessInformationClass*/, LPVOID /*ProcessInformation*/, DWORD /*ProcessInformationSize*/) {
    return TRUE;
}

// TaskDialogIndirect
#define TDF_ALLOW_DIALOG_CANCELLATION 0x0008
#define TDF_SIZE_TO_CONTENT 0x01000000
#define TDCBF_OK_BUTTON 0x0001
#define TDCBF_YES_BUTTON 0x0002
#define TDCBF_NO_BUTTON 0x0004
#define TDCBF_CANCEL_BUTTON 0x0008
#define TDCBF_RETRY_BUTTON 0x0010
#define TDCBF_CLOSE_BUTTON 0x0020

struct TASKDIALOG_BUTTON {
    int nButtonID;
    PCWSTR pszButtonText;
};

struct TASKDIALOGCONFIG {
    UINT cbSize;
    HWND hwndParent;
    HINSTANCE hInstance;
    DWORD dwFlags;
    DWORD dwCommonButtons;
    PCWSTR pszWindowTitle;
    union {
        HICON hMainIcon;
        PCWSTR pszMainIcon;
    };
    PCWSTR pszMainInstruction;
    PCWSTR pszContent;
    UINT cButtons;
    const TASKDIALOG_BUTTON* pButtons;
    int nDefaultButton;
    UINT cRadioButtons;
    const TASKDIALOG_BUTTON* pRadioButtons;
    int nDefaultRadioButton;
    PCWSTR pszVerificationText;
    PCWSTR pszExpandedInformation;
    PCWSTR pszExpandedControlText;
    PCWSTR pszCollapsedControlText;
    union {
        HICON hFooterIcon;
        PCWSTR pszFooterIcon;
    };
    PCWSTR pszFooter;
    void* pfCallback;
    LONG_PTR lpCallbackData;
    UINT cxWidth;
};

inline HRESULT TaskDialogIndirect(const TASKDIALOGCONFIG* pTaskConfig, int* pnButton, int* pnRadioButton, BOOL* pfVerificationFlagChecked) {
    if (pnRadioButton) *pnRadioButton = 0;
    if (pfVerificationFlagChecked) *pfVerificationFlagChecked = FALSE;
    if (!pTaskConfig) return E_INVALIDARG;
    // Delegate to MessageBoxW fallback so callers get native dialog prompt
    return E_NOTIMPL;
}

using WNDENUMPROC = BOOL(CALLBACK*)(HWND, LPARAM);
inline HWND FindWindowW(LPCWSTR /*lpClassName*/, LPCWSTR /*lpWindowName*/) { return nullptr; }
inline HWND FindWindowExW(HWND /*hWndParent*/, HWND /*hWndChildAfter*/, LPCWSTR /*lpszClass*/, LPCWSTR /*lpszWindow*/) { return nullptr; }
inline BOOL EnumWindows(WNDENUMPROC /*lpEnumFunc*/, LPARAM /*lParam*/) { return TRUE; }
inline int GetClassNameW(HWND /*hWnd*/, LPWSTR lpClassName, int nMaxCount) {
    if (lpClassName && nMaxCount > 0) lpClassName[0] = L'\0';
    return 0;
}

#ifndef TRANSPARENT
#define TRANSPARENT 1
#endif
#ifndef OPAQUE
#define OPAQUE 2
#endif
#ifndef DEFAULT_GUI_FONT
#define DEFAULT_GUI_FONT 17
#endif
#ifndef NULL_BRUSH
#define NULL_BRUSH 5
#endif
#ifndef BLACK_BRUSH
#define BLACK_BRUSH 4
#endif

inline int SetBkMode(HDC /*hdc*/, int /*mode*/) { return 1; }
inline COLORREF SetTextColor(HDC /*hdc*/, COLORREF /*color*/) { return 0; }
inline COLORREF SetBkColor(HDC /*hdc*/, COLORREF /*color*/) { return 0; }
inline HGDIOBJ GetStockObject(int /*i*/) { return reinterpret_cast<HGDIOBJ>(static_cast<uintptr_t>(0x1)); }
inline BOOL GetTextExtentPoint32W(HDC /*hdc*/, LPCWSTR /*lpString*/, int c, LPSIZE psizl) {
    if (psizl) {
        psizl->cx = (c > 0 ? c : 0) * 8;
        psizl->cy = 16;
    }
    return TRUE;
}
inline BOOL TextOutW(HDC /*hdc*/, int /*x*/, int /*y*/, LPCWSTR /*lpString*/, int /*c*/) { return TRUE; }

#endif // !defined(_WIN32)
