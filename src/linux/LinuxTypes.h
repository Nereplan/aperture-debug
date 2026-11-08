#pragma once

#if !defined(_WIN32)

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <cstdarg>
#include <cmath>
#include <ctime>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <charconv>
#include <cerrno>

#if defined(_LIBCPP_VERSION)
namespace std {
inline from_chars_result from_chars(const char* first, const char* last, double& value, chars_format /*fmt*/ = chars_format::general) noexcept {
    if (!first || first >= last) return {first, errc::invalid_argument};
    char buf[128];
    size_t len = static_cast<size_t>(last - first);
    std::string heapBuf;
    const char* cstr = nullptr;
    if (len < sizeof(buf)) {
        std::memcpy(buf, first, len);
        buf[len] = '\0';
        cstr = buf;
    } else {
        heapBuf.assign(first, len);
        cstr = heapBuf.c_str();
    }
    int savedErrno = errno;
    errno = 0;
    char* endPtr = nullptr;
    double parsed = std::strtod(cstr, &endPtr);
    int err = errno;
    errno = savedErrno;
    if (endPtr == cstr) return {first, errc::invalid_argument};
    const char* actualEnd = first + (endPtr - cstr);
    if (err == ERANGE) return {actualEnd, errc::result_out_of_range};
    value = parsed;
    return {actualEnd, errc{}};
}
} // namespace std
#endif

#define WINAPI
#define CALLBACK
#define APIENTRY
#define STDMETHODCALLTYPE
#define __stdcall
#define __cdecl
#define __fastcall
#define DECLSPEC_UUID(x)
#define MIDL_INTERFACE(x) struct

#ifndef _In_
#define _In_
#define _In_opt_
#define _In_z_
#define _In_opt_z_
#define _In_reads_(x)
#define _In_reads_bytes_(x)
#define _In_reads_opt_(x)
#define _Out_
#define _Out_opt_
#define _Out_writes_(x)
#define _Out_writes_bytes_(x)
#define _Out_writes_opt_(x)
#define _Out_writes_to_opt_(x, y)
#define _Inout_
#define _Inout_opt_
#define _Outptr_
#define _Outptr_opt_
#define _Outptr_result_maybenull_
#define _COM_Outptr_
#define _Check_return_
#define __RPC__in
#define __RPC__out
#define __RPC__inout
#define __RPC__in_opt
#define __RPC__deref_out_opt
#endif

using BYTE = uint8_t;
using WORD = uint16_t;
using DWORD = uint32_t;
using UINT = unsigned int;
using INT = int;
using LONG = int32_t;
using ULONG = uint32_t;
using LONGLONG = int64_t;
using ULONGLONG = uint64_t;
using SHORT = int16_t;
using USHORT = uint16_t;
using BOOL = int;
using BOOLEAN = uint8_t;
using CHAR = char;
using WCHAR = wchar_t;
using UCHAR = unsigned char;
using FLOAT = float;
using DOUBLE = double;

using INT8 = int8_t;
using UINT8 = uint8_t;
using INT16 = int16_t;
using UINT16 = uint16_t;
using INT32 = int32_t;
using UINT32 = uint32_t;
using INT64 = int64_t;
using UINT64 = uint64_t;

using INT_PTR = intptr_t;
using UINT_PTR = uintptr_t;
using LONG_PTR = intptr_t;
using ULONG_PTR = uintptr_t;
using DWORD_PTR = uintptr_t;
using SIZE_T = size_t;
using SSIZE_T = intptr_t;

using LPSTR = char*;
using LPCSTR = const char*;
using LPWSTR = wchar_t*;
using LPCWSTR = const wchar_t*;
using PCWSTR = const wchar_t*;
using PWSTR = wchar_t*;
using LPTSTR = wchar_t*;
using LPCTSTR = const wchar_t*;
using LPVOID = void*;
using LPCVOID = const void*;
using PVOID = void*;
using VOID = void;
using PBYTE = uint8_t*;
using LPBYTE = uint8_t*;
using PUCHAR = uint8_t*;
using LPDWORD = uint32_t*;
using PDWORD = uint32_t*;
using LPBOOL = int*;
using PULONG = uint32_t*;
using ULONG64 = uint64_t;
using DWORD64 = uint64_t;

using HRESULT = int32_t;
using NTSTATUS = int32_t;
using LRESULT = intptr_t;
using WPARAM = uintptr_t;
using LPARAM = intptr_t;
using ATOM = uint16_t;
using COLORREF = uint32_t;
using LANGID = uint16_t;
using LCID = uint32_t;
using LCTYPE = uint32_t;
using REGSAM = uint32_t;
using ACCESS_MASK = uint32_t;
using EXECUTION_STATE = uint32_t;
using CLIPFORMAT = uint16_t;
using VARTYPE = uint16_t;
using PROPID = uint32_t;

using HANDLE = void*;
using LPHANDLE = HANDLE*;
using PHANDLE = HANDLE*;
using HINSTANCE = void*;
using HMODULE = void*;
using HWND = void*;
using HDC = void*;
using HGDIOBJ = void*;
using HBITMAP = void*;
using HBRUSH = void*;
using HPEN = void*;
using HFONT = void*;
using HICON = void*;
using HCURSOR = void*;
using HMENU = void*;
using HMONITOR = void*;
using HGLOBAL = void*;
using HLOCAL = void*;
using HRGN = void*;
using HACCEL = void*;
using HDROP = void*;
using HIMC = void*;
using HKEY = void*;
using HINTERNET = void*;
using BCRYPT_ALG_HANDLE = void*;
using BCRYPT_HASH_HANDLE = void*;
using BCRYPT_KEY_HANDLE = void*;
using BCRYPT_HANDLE = void*;
using HSTRING = void*;
using HTHEME = void*;
using HWINEVENTHOOK = void*;

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)

#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_POINTER ((HRESULT)0x80004003L)
#define E_ABORT ((HRESULT)0x80004004L)
#define E_ACCESSDENIED ((HRESULT)0x80070005L)
#define E_PENDING ((HRESULT)0x8000000AL)
#define E_BOUNDS ((HRESULT)0x8000000BL)
#define E_CHANGED_STATE ((HRESULT)0x8000000CL)
#define RPC_E_CHANGED_MODE ((HRESULT)0x80010106L)
#define D2DERR_RECREATE_TARGET ((HRESULT)0x8899000CL)
#define D2DERR_OUTSTANDING_BITMAP_REFERENCES ((HRESULT)0x88990020L)
#define DXGI_ERROR_DEVICE_REMOVED ((HRESULT)0x887A0005L)
#define DXGI_ERROR_DEVICE_RESET ((HRESULT)0x887A0007L)
#define MF_E_END_OF_STREAM ((HRESULT)0xC00D3E84L)
#define MF_E_NO_MORE_TYPES ((HRESULT)0xC00D36B9L)
#define MF_E_INVALIDMEDIATYPE ((HRESULT)0xC00D36B4L)
#define MF_E_TOPO_CODEC_NOT_FOUND ((HRESULT)0xC00D5212L)

constexpr bool SUCCEEDED(HRESULT hr) noexcept { return hr >= 0; }
constexpr bool FAILED(HRESULT hr) noexcept { return hr < 0; }
constexpr HRESULT HRESULT_FROM_WIN32(DWORD x) noexcept {
    return (HRESULT)(x) <= 0 ? (HRESULT)(x) : (HRESULT)(((x) & 0x0000FFFF) | (7 << 16) | 0x80000000);
}
#define BCRYPT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif

#ifndef ARRAYSIZE
template <typename T, size_t N>
constexpr size_t _LinuxArraySize(T (&)[N]) noexcept { return N; }
#define ARRAYSIZE(a) _LinuxArraySize(a)
#endif

#ifndef RGB
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(rgb) ((BYTE)(rgb))
#define GetGValue(rgb) ((BYTE)(((WORD)(rgb)) >> 8))
#define GetBValue(rgb) ((BYTE)((rgb) >> 16))
#endif

#ifndef LOWORD
#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define HIBYTE(w) ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xff))
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)(((WORD)(((DWORD_PTR)(l)) & 0xffff)) | ((DWORD)((WORD)(((DWORD_PTR)(h)) & 0xffff))) << 16))
#define MAKEWPARAM(l, h) ((WPARAM)(DWORD)(((WORD)(((DWORD_PTR)(l)) & 0xffff)) | ((DWORD)((WORD)(((DWORD_PTR)(h)) & 0xffff))) << 16))
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#define GET_WHEEL_DELTA_WPARAM(wParam) ((short)HIWORD(wParam))
#define GET_KEYSTATE_WPARAM(wParam) (LOWORD(wParam))
#define GET_XBUTTON_WPARAM(wParam) (HIWORD(wParam))
#define MAKEINTRESOURCEW(i) ((LPWSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCE(i) MAKEINTRESOURCEW(i)
#define IS_INTRESOURCE(_r) ((((ULONG_PTR)(_r)) >> 16) == 0)
#endif

#define ZeroMemory(Destination, Length) memset((Destination), 0, (Length))
#define CopyMemory(Destination, Source, Length) memcpy((Destination), (Source), (Length))
#define MoveMemory(Destination, Source, Length) memmove((Destination), (Source), (Length))
#define SecureZeroMemory(Destination, Length) memset((Destination), 0, (Length))

inline int _wtoi(const wchar_t* s) noexcept { return s ? static_cast<int>(std::wcstol(s, nullptr, 10)) : 0; }
inline double _wtof(const wchar_t* s) noexcept { return s ? std::wcstod(s, nullptr) : 0.0; }
inline int64_t _wtoi64(const wchar_t* s) noexcept { return s ? static_cast<int64_t>(std::wcstoll(s, nullptr, 10)) : 0; }

// SEH compatibility on GCC/Clang
#ifndef __try
#define __try if (true)
#define __except(x) else
#define EXCEPTION_EXECUTE_HANDLER 1
#endif

struct GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
    constexpr bool operator==(const GUID& o) const noexcept {
        return Data1 == o.Data1 && Data2 == o.Data2 && Data3 == o.Data3 &&
               Data4[0] == o.Data4[0] && Data4[1] == o.Data4[1] &&
               Data4[2] == o.Data4[2] && Data4[3] == o.Data4[3] &&
               Data4[4] == o.Data4[4] && Data4[5] == o.Data4[5] &&
               Data4[6] == o.Data4[6] && Data4[7] == o.Data4[7];
    }
    constexpr bool operator!=(const GUID& o) const noexcept { return !(*this == o); }
};
using IID = GUID;
using CLSID = GUID;
using REFGUID = const GUID&;
using REFIID = const IID&;
using REFCLSID = const CLSID&;

inline constexpr bool IsEqualGUID(REFGUID rguid1, REFGUID rguid2) noexcept { return rguid1 == rguid2; }
inline constexpr bool IsEqualIID(REFIID riid1, REFIID riid2) noexcept { return riid1 == riid2; }
inline constexpr bool IsEqualCLSID(REFCLSID rclsid1, REFCLSID rclsid2) noexcept { return rclsid1 == rclsid2; }

inline constexpr GUID GUID_NULL = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};

template <typename T>
constexpr GUID __LinuxMakeTypeGuid() noexcept {
    const char* s = __PRETTY_FUNCTION__;
    uint64_t h1 = 14695981039346656037ULL;
    uint64_t h2 = 1099511628211ULL;
    for (size_t i = 0; s[i] != '\0'; ++i) {
        h1 = (h1 ^ static_cast<uint8_t>(s[i])) * 1099511628211ULL;
        h2 = ((h2 ^ static_cast<uint8_t>(s[i])) + i) * 14695981039346656037ULL;
    }
    return GUID{
        static_cast<uint32_t>(h1 >> 32),
        static_cast<uint16_t>((h1 >> 16) & 0xFFFF),
        static_cast<uint16_t>(h1 & 0xFFFF),
        {
            static_cast<uint8_t>((h2 >> 56) & 0xFF),
            static_cast<uint8_t>((h2 >> 48) & 0xFF),
            static_cast<uint8_t>((h2 >> 40) & 0xFF),
            static_cast<uint8_t>((h2 >> 32) & 0xFF),
            static_cast<uint8_t>((h2 >> 24) & 0xFF),
            static_cast<uint8_t>((h2 >> 16) & 0xFF),
            static_cast<uint8_t>((h2 >> 8) & 0xFF),
            static_cast<uint8_t>(h2 & 0xFF)
        }
    };
}

template <typename T>
struct __LinuxUuidOf {
    static constexpr GUID value = __LinuxMakeTypeGuid<T>();
};
#define __uuidof(T) (__LinuxUuidOf<std::remove_cv_t<std::remove_reference_t<std::remove_pointer_t<T>>>>::value)

struct POINT {
    LONG x;
    LONG y;
};
using LPPOINT = POINT*;

struct POINTL {
    LONG x;
    LONG y;
};

struct SIZE {
    LONG cx;
    LONG cy;
};
using LPSIZE = SIZE*;

struct RECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
};
using LPRECT = RECT*;
using LPCRECT = const RECT*;

union LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    struct {
        DWORD LowPart;
        LONG HighPart;
    } u;
    LONGLONG QuadPart;
};
using PLARGE_INTEGER = LARGE_INTEGER*;

union ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    struct {
        DWORD LowPart;
        DWORD HighPart;
    } u;
    ULONGLONG QuadPart;
};
using PULARGE_INTEGER = ULARGE_INTEGER*;

struct FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
};
using LPFILETIME = FILETIME*;

struct SYSTEMTIME {
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
};
using LPSYSTEMTIME = SYSTEMTIME*;

struct SECURITY_ATTRIBUTES {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
};
using LPSECURITY_ATTRIBUTES = SECURITY_ATTRIBUTES*;

// Base COM IUnknown & ComPtr
struct IUnknown {
    virtual ~IUnknown() = default;
    virtual HRESULT QueryInterface(REFIID riid, void** ppvObject) {
        if (!ppvObject) return E_POINTER;
        *ppvObject = this;
        AddRef();
        return S_OK;
    }
    virtual ULONG AddRef() {
        return ++m_refCount;
    }
    virtual ULONG Release() {
        ULONG r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }
protected:
    std::atomic<ULONG> m_refCount{1};
};

struct IInspectable : public IUnknown {};
using LPUNKNOWN = IUnknown*;

namespace Microsoft {
namespace WRL {

template <typename T>
class ComPtr {
public:
    using InterfaceType = T;

    ComPtr() noexcept : ptr_(nullptr) {}
    ComPtr(std::nullptr_t) noexcept : ptr_(nullptr) {}
    template <typename U>
    ComPtr(U* other) noexcept : ptr_(other) {
        InternalAddRef();
    }
    ComPtr(const ComPtr& other) noexcept : ptr_(other.ptr_) {
        InternalAddRef();
    }
    template <typename U>
    ComPtr(const ComPtr<U>& other) noexcept : ptr_(other.Get()) {
        InternalAddRef();
    }
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }
    template <typename U>
    ComPtr(ComPtr<U>&& other) noexcept : ptr_(other.Detach()) {}

    ~ComPtr() noexcept {
        InternalRelease();
    }

    ComPtr& operator=(std::nullptr_t) noexcept {
        InternalRelease();
        return *this;
    }
    ComPtr& operator=(T* other) noexcept {
        if (ptr_ != other) {
            T* old = ptr_;
            ptr_ = other;
            InternalAddRef();
            if (old) old->Release();
        }
        return *this;
    }
    ComPtr& operator=(const ComPtr& other) noexcept {
        return operator=(other.ptr_);
    }
    template <typename U>
    ComPtr& operator=(const ComPtr<U>& other) noexcept {
        return operator=( static_cast<T*>(other.Get()) );
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != reinterpret_cast<ComPtr*>(&other)) {
            InternalRelease();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }
    template <typename U>
    ComPtr& operator=(ComPtr<U>&& other) noexcept {
        InternalRelease();
        ptr_ = other.Detach();
        return *this;
    }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    T& operator*() const noexcept { return *ptr_; }

    T* const* GetAddressOf() const noexcept { return &ptr_; }
    T** GetAddressOf() noexcept { return &ptr_; }
    T** ReleaseAndGetAddressOf() noexcept {
        InternalRelease();
        return &ptr_;
    }
    T** operator&() noexcept {
        return ReleaseAndGetAddressOf();
    }
    T* const* operator&() const noexcept {
        return GetAddressOf();
    }

    T* Detach() noexcept {
        T* tmp = ptr_;
        ptr_ = nullptr;
        return tmp;
    }
    void Attach(T* other) noexcept {
        if (ptr_ != nullptr) {
            ptr_->Release();
        }
        ptr_ = other;
    }
    ULONG Reset() {
        return InternalRelease();
    }

    template <typename U>
    HRESULT As(ComPtr<U>* p) const noexcept {
        if (!p) return E_POINTER;
        if (!ptr_) {
            p->Reset();
            return E_POINTER;
        }
        U* raw = nullptr;
        HRESULT hr = ptr_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(&raw));
        if (SUCCEEDED(hr) && !raw) {
            raw = reinterpret_cast<U*>(ptr_);
            raw->AddRef();
        }
        p->Attach(raw);
        return hr;
    }

    template <typename U>
    HRESULT As(U** p) const noexcept {
        if (!p) return E_POINTER;
        *p = nullptr;
        if (!ptr_) return E_POINTER;
        HRESULT hr = ptr_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(p));
        if (SUCCEEDED(hr) && !*p) {
            *p = reinterpret_cast<U*>(ptr_);
            (*p)->AddRef();
        }
        return hr;
    }

    HRESULT CopyTo(REFIID riid, void** ptr) const noexcept {
        if (!ptr) return E_POINTER;
        if (!ptr_) { *ptr = nullptr; return E_POINTER; }
        return ptr_->QueryInterface(riid, ptr);
    }

    template <typename U>
    HRESULT CopyTo(U** ptr) const noexcept {
        if (!ptr) return E_POINTER;
        if (!ptr_) { *ptr = nullptr; return E_POINTER; }
        *ptr = static_cast<U*>(ptr_);
        ptr_->AddRef();
        return S_OK;
    }

private:
    void InternalAddRef() const noexcept {
        if (ptr_) ptr_->AddRef();
    }
    ULONG InternalRelease() noexcept {
        ULONG r = 0;
        T* tmp = ptr_;
        if (tmp) {
            ptr_ = nullptr;
            r = tmp->Release();
        }
        return r;
    }
    T* ptr_;
};

template <typename T, typename U>
bool operator==(const ComPtr<T>& a, const ComPtr<U>& b) noexcept { return a.Get() == b.Get(); }
template <typename T>
bool operator==(const ComPtr<T>& a, std::nullptr_t) noexcept { return a.Get() == nullptr; }
template <typename T>
bool operator==(std::nullptr_t, const ComPtr<T>& a) noexcept { return a.Get() == nullptr; }
template <typename T, typename U>
bool operator!=(const ComPtr<T>& a, const ComPtr<U>& b) noexcept { return a.Get() != b.Get(); }
template <typename T>
bool operator!=(const ComPtr<T>& a, std::nullptr_t) noexcept { return a.Get() != nullptr; }
template <typename T>
bool operator!=(std::nullptr_t, const ComPtr<T>& a) noexcept { return a.Get() != nullptr; }

template <typename T>
struct RuntimeClassFlags {};
template <typename... Args>
class RuntimeClass : public IUnknown {};
template <typename T, typename... Args>
ComPtr<T> Make(Args&&... args) {
    ComPtr<T> p;
    p.Attach(new T(std::forward<Args>(args)...));
    return p;
}

namespace Wrappers {
class HStringReference {
public:
    HStringReference(const wchar_t* str) : m_str(str ? str : L"") {}
    HSTRING Get() const noexcept { return (HSTRING)m_str.c_str(); }
private:
    std::wstring m_str;
};
class HString {
public:
    HString() = default;
    void Release() noexcept { m_str.clear(); }
    HRESULT Set(const wchar_t* str) noexcept { m_str = str ? str : L""; return S_OK; }
    HSTRING Get() const noexcept { return (HSTRING)m_str.c_str(); }
    HSTRING* GetAddressOf() noexcept { return &m_handle; }
    const wchar_t* GetRawBuffer(uint32_t* length) const noexcept {
        if (length) *length = (uint32_t)m_str.size();
        return m_str.c_str();
    }
private:
    std::wstring m_str;
    HSTRING m_handle = nullptr;
};
} // namespace Wrappers

} // namespace WRL
} // namespace Microsoft

template <typename T>
void** IID_PPV_ARGS_Helper(Microsoft::WRL::ComPtr<T>* pp) noexcept {
    return reinterpret_cast<void**>(pp->ReleaseAndGetAddressOf());
}
template <typename T>
void** IID_PPV_ARGS_Helper(T** pp) noexcept {
    return reinterpret_cast<void**>(pp);
}
#define IID_PPV_ARGS(ppType) __uuidof(decltype(**(ppType))), IID_PPV_ARGS_Helper(ppType)

#endif // !defined(_WIN32)
