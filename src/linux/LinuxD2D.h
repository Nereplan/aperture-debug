#pragma once

#if !defined(_WIN32)

#include "LinuxTypes.h"

// ============================================================================
// DXGI & D3D11 Types & Interfaces
// ============================================================================

enum DXGI_FORMAT {
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_B8G8R8A8_UNORM = 87,
};

enum D2D1_ALPHA_MODE {
    D2D1_ALPHA_MODE_UNKNOWN = 0,
    D2D1_ALPHA_MODE_PREMULTIPLIED = 1,
    D2D1_ALPHA_MODE_STRAIGHT = 2,
    D2D1_ALPHA_MODE_IGNORE = 3,
};

struct DXGI_SAMPLE_DESC {
    UINT Count;
    UINT Quality;
};

enum DXGI_SCALING {
    DXGI_SCALING_STRETCH = 0,
    DXGI_SCALING_NONE = 1,
    DXGI_SCALING_ASPECT_RATIO_STRETCH = 2,
};

enum DXGI_SWAP_EFFECT {
    DXGI_SWAP_EFFECT_DISCARD = 0,
    DXGI_SWAP_EFFECT_SEQUENTIAL = 1,
    DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3,
    DXGI_SWAP_EFFECT_FLIP_DISCARD = 4,
};

enum DXGI_ALPHA_MODE {
    DXGI_ALPHA_MODE_UNSPECIFIED = 0,
    DXGI_ALPHA_MODE_PREMULTIPLIED = 1,
    DXGI_ALPHA_MODE_STRAIGHT = 2,
    DXGI_ALPHA_MODE_IGNORE = 3,
};

#define DXGI_USAGE_RENDER_TARGET_OUTPUT 0x00000020UL
#define DXGI_MWA_NO_ALT_ENTER (1 << 1)
#define DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT 0x40
#define DXGI_STATUS_OCCLUDED ((HRESULT)0x087A0001L)
#define DXGI_ERROR_DRIVER_INTERNAL_ERROR ((HRESULT)0x887A0020L)
#define DXGI_ERROR_DEVICE_HUNG ((HRESULT)0x887A0006L)

struct LUID {
    DWORD LowPart;
    LONG HighPart;
};

struct DXGI_ADAPTER_DESC {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    LUID AdapterLuid;
};

struct DXGI_SWAP_CHAIN_DESC1 {
    UINT Width;
    UINT Height;
    DXGI_FORMAT Format;
    BOOL Stereo;
    DXGI_SAMPLE_DESC SampleDesc;
     UINT BufferUsage;
    UINT BufferCount;
    DXGI_SCALING Scaling;
    DXGI_SWAP_EFFECT SwapEffect;
    DXGI_ALPHA_MODE AlphaMode;
    UINT Flags;
};

struct DXGI_PRESENT_PARAMETERS {
    UINT DirtyRectsCount;
    RECT* pDirtyRects;
    RECT* pScrollRect;
    POINT* pScrollOffset;
};

struct IDXGIObject : public IUnknown {};
struct IDXGIDeviceSubObject : public IDXGIObject {};

struct IDXGISurface : public IDXGIDeviceSubObject {
    UINT m_width = 1280;
    UINT m_height = 720;
    std::vector<uint8_t> m_pixels;
    IDXGISurface(UINT w = 1280, UINT h = 720) : m_width(w ? w : 1280), m_height(h ? h : 720) {
        m_pixels.resize((size_t)m_width * m_height * 4, 0);
    }
};

struct IDXGIAdapter : public IDXGIObject {
    virtual HRESULT GetParent(REFIID riid, void** ppParent);
    virtual HRESULT GetDesc(DXGI_ADAPTER_DESC* pDesc) {
        if (!pDesc) return E_POINTER;
        std::memset(pDesc, 0, sizeof(*pDesc));
        std::wcscpy(pDesc->Description, L"Linux Cairo/SDL2 Graphics Adapter");
        pDesc->VendorId = 0x1002;
        pDesc->DeviceId = 0x1000;
        return S_OK;
    }
};

struct IDXGIFactory2 : public IDXGIObject {
    virtual HRESULT CreateSwapChainForHwnd(
        IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
        const void* pFullscreenDesc, IDXGIObject* pRestrictToOutput,
        struct IDXGISwapChain1** ppSwapChain);
    virtual HRESULT MakeWindowAssociation(HWND WindowHandle, UINT Flags) { return S_OK; }
};

inline HRESULT IDXGIAdapter::GetParent(REFIID /*riid*/, void** ppParent) {
    if (!ppParent) return E_POINTER;
    *ppParent = new IDXGIFactory2();
    return S_OK;
}

struct IDXGIDevice : public IDXGIObject {
    virtual HRESULT GetAdapter(IDXGIAdapter** pAdapter) {
        if (!pAdapter) return E_POINTER;
        *pAdapter = new IDXGIAdapter();
        return S_OK;
    }
};

struct IDXGIDevice1 : public IDXGIDevice {
    virtual HRESULT SetMaximumFrameLatency(UINT MaxLatency) { return S_OK; }
};

struct IDXGISwapChain1 : public IDXGIDeviceSubObject {
    HWND m_hwnd = nullptr;
    UINT m_width = 1280;
    UINT m_height = 720;
    Microsoft::WRL::ComPtr<IDXGISurface> m_surface;

    IDXGISwapChain1(HWND hwnd = nullptr, UINT w = 1280, UINT h = 720)
        : m_hwnd(hwnd), m_width(w ? w : 1280), m_height(h ? h : 720) {
        m_surface.Attach(new IDXGISurface(m_width, m_height));
    }
    virtual HRESULT GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) {
        if (!ppSurface) return E_POINTER;
        if (!m_surface) m_surface.Attach(new IDXGISurface(m_width, m_height));
        *ppSurface = m_surface.Get();
        m_surface->AddRef();
        return S_OK;
    }
    virtual HRESULT ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
        if (Width > 0) m_width = Width;
        if (Height > 0) m_height = Height;
        m_surface.Attach(new IDXGISurface(m_width, m_height));
        return S_OK;
    }
    virtual HRESULT Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
    virtual HRESULT Present(UINT SyncInterval, UINT Flags) {
        return Present1(SyncInterval, Flags, nullptr);
    }
};

enum D3D_DRIVER_TYPE {
    D3D_DRIVER_TYPE_UNKNOWN = 0,
    D3D_DRIVER_TYPE_HARDWARE = 1,
    D3D_DRIVER_TYPE_REFERENCE = 2,
    D3D_DRIVER_TYPE_NULL = 3,
    D3D_DRIVER_TYPE_SOFTWARE = 4,
    D3D_DRIVER_TYPE_WARP = 5,
};

enum D3D_FEATURE_LEVEL {
    D3D_FEATURE_LEVEL_9_1 = 0x9100,
    D3D_FEATURE_LEVEL_9_2 = 0x9200,
    D3D_FEATURE_LEVEL_9_3 = 0x9300,
    D3D_FEATURE_LEVEL_10_0 = 0xa000,
    D3D_FEATURE_LEVEL_10_1 = 0xa100,
    D3D_FEATURE_LEVEL_11_0 = 0xb000,
    D3D_FEATURE_LEVEL_11_1 = 0xb100,
};

#define D3D11_CREATE_DEVICE_BGRA_SUPPORT 0x20
#define D3D11_SDK_VERSION 7
#define D3D11_USAGE_STAGING 3
#define D3D11_CPU_ACCESS_READ 0x20000L
#define D3D11_MAP_READ 1

struct D3D11_TEXTURE2D_DESC {
    UINT Width;
    UINT Height;
    UINT MipLevels;
    UINT ArraySize;
    DXGI_FORMAT Format;
    DXGI_SAMPLE_DESC SampleDesc;
    UINT Usage;
    UINT BindFlags;
    UINT CPUAccessFlags;
    UINT MiscFlags;
};

struct D3D11_MAPPED_SUBRESOURCE {
    void* pData;
    UINT RowPitch;
    UINT DepthPitch;
};

struct ID3D11Resource : public IUnknown {};

struct ID3D11Texture2D : public IDXGISurface {
    using IDXGISurface::IDXGISurface;
    virtual void GetDesc(D3D11_TEXTURE2D_DESC* pDesc) {
        if (!pDesc) return;
        std::memset(pDesc, 0, sizeof(*pDesc));
        pDesc->Width = m_width;
        pDesc->Height = m_height;
        pDesc->Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }
};

struct ID3D11DeviceContext : public IUnknown {
    virtual void ClearState() {}
    virtual void Flush() {}
    virtual void CopyResource(IUnknown* pDstResource, IUnknown* pSrcResource) {
        auto* dst = static_cast<ID3D11Texture2D*>(pDstResource);
        auto* src = static_cast<ID3D11Texture2D*>(pSrcResource);
        if (dst && src) {
            dst->m_width = src->m_width;
            dst->m_height = src->m_height;
            dst->m_pixels = src->m_pixels;
        }
    }
    virtual HRESULT Map(IUnknown* pResource, UINT /*Subresource*/, UINT /*MapType*/, UINT /*MapFlags*/, D3D11_MAPPED_SUBRESOURCE* pMappedResource) {
        if (!pResource || !pMappedResource) return E_POINTER;
        auto* tex = static_cast<ID3D11Texture2D*>(pResource);
        pMappedResource->pData = tex->m_pixels.data();
        pMappedResource->RowPitch = tex->m_width * 4;
        pMappedResource->DepthPitch = tex->m_width * tex->m_height * 4;
        return S_OK;
    }
    virtual void Unmap(IUnknown* /*pResource*/, UINT /*Subresource*/) {}
};

struct ID3D11Device : public IDXGIDevice1 {
    virtual HRESULT GetDeviceRemovedReason() { return S_OK; }
    virtual HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC* pDesc, const void* /*pInitialData*/, ID3D11Texture2D** ppTexture2D) {
        if (!ppTexture2D) return E_POINTER;
        UINT w = pDesc ? pDesc->Width : 1280;
        UINT h = pDesc ? pDesc->Height : 720;
        *ppTexture2D = new ID3D11Texture2D(w, h);
        return S_OK;
    }
};

inline HRESULT D3D11CreateDevice(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext) {
    if (ppDevice) *ppDevice = new ID3D11Device();
    if (pFeatureLevel) *pFeatureLevel = D3D_FEATURE_LEVEL_11_1;
    if (ppImmediateContext) *ppImmediateContext = new ID3D11DeviceContext();
    return S_OK;
}

inline HRESULT IDXGIFactory2::CreateSwapChainForHwnd(
    IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const void* pFullscreenDesc, IDXGIObject* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) {
    if (!ppSwapChain) return E_POINTER;
    UINT w = (pDesc && pDesc->Width > 0) ? pDesc->Width : 1280;
    UINT h = (pDesc && pDesc->Height > 0) ? pDesc->Height : 720;
    *ppSwapChain = new IDXGISwapChain1(hWnd, w, h);
    return S_OK;
}

// ============================================================================
// Direct2D Types & Interfaces
// ============================================================================

struct D2D1_COLOR_F {
    FLOAT r;
    FLOAT g;
    FLOAT b;
    FLOAT a;
};

struct D2D1_POINT_2F {
    FLOAT x;
    FLOAT y;
};

struct D2D1_SIZE_F {
    FLOAT width;
    FLOAT height;
};

struct D2D1_SIZE_U {
    UINT32 width;
    UINT32 height;
};

struct D2D1_RECT_F {
    FLOAT left;
    FLOAT top;
    FLOAT right;
    FLOAT bottom;
};

struct D2D1_RECT_U {
    UINT32 left;
    UINT32 top;
    UINT32 right;
    UINT32 bottom;
};

struct D2D1_ROUNDED_RECT {
    D2D1_RECT_F rect;
    FLOAT radiusX;
    FLOAT radiusY;
};

struct D2D1_ELLIPSE {
    D2D1_POINT_2F point;
    FLOAT radiusX;
    FLOAT radiusY;
};

struct D2D1_PIXEL_FORMAT {
    DXGI_FORMAT format;
    D2D1_ALPHA_MODE alphaMode;
};

struct D2D1_MATRIX_3X2_F {
    union {
        struct {
            FLOAT m11, m12;
            FLOAT m21, m22;
            FLOAT dx, dy;
        };
        struct {
            FLOAT _11, _12;
            FLOAT _21, _22;
            FLOAT _31, _32;
        };
        FLOAT m[3][2];
    };
};

inline constexpr D2D1_MATRIX_3X2_F operator*(const D2D1_MATRIX_3X2_F& a, const D2D1_MATRIX_3X2_F& b) {
    D2D1_MATRIX_3X2_F r{};
    r.m11 = a.m11 * b.m11 + a.m12 * b.m21;
    r.m12 = a.m11 * b.m12 + a.m12 * b.m22;
    r.m21 = a.m21 * b.m11 + a.m22 * b.m21;
    r.m22 = a.m21 * b.m12 + a.m22 * b.m22;
    r.dx  = a.dx  * b.m11 + a.dy  * b.m21 + b.dx;
    r.dy  = a.dx  * b.m12 + a.dy  * b.m22 + b.dy;
    return r;
}

enum D2D1_FACTORY_TYPE {
    D2D1_FACTORY_TYPE_SINGLE_THREADED = 0,
    D2D1_FACTORY_TYPE_MULTI_THREADED = 1,
};

enum D2D1_RENDER_TARGET_TYPE {
    D2D1_RENDER_TARGET_TYPE_DEFAULT = 0,
    D2D1_RENDER_TARGET_TYPE_SOFTWARE = 1,
    D2D1_RENDER_TARGET_TYPE_HARDWARE = 2,
};

enum D2D1_RENDER_TARGET_USAGE {
    D2D1_RENDER_TARGET_USAGE_NONE = 0,
    D2D1_RENDER_TARGET_USAGE_FORCE_BITMAP_REMOTING = 1,
    D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE = 2,
};

enum D2D1_FEATURE_LEVEL {
    D2D1_FEATURE_LEVEL_DEFAULT = 0,
    D2D1_FEATURE_LEVEL_9 = D3D_FEATURE_LEVEL_9_1,
    D2D1_FEATURE_LEVEL_10 = D3D_FEATURE_LEVEL_10_0,
};

struct D2D1_RENDER_TARGET_PROPERTIES {
    D2D1_RENDER_TARGET_TYPE type;
    D2D1_PIXEL_FORMAT pixelFormat;
    FLOAT dpiX;
    FLOAT dpiY;
    D2D1_RENDER_TARGET_USAGE usage;
    D2D1_FEATURE_LEVEL minLevel;
};

enum D2D1_BITMAP_OPTIONS {
    D2D1_BITMAP_OPTIONS_NONE = 0x00000000,
    D2D1_BITMAP_OPTIONS_TARGET = 0x00000001,
    D2D1_BITMAP_OPTIONS_CANNOT_DRAW = 0x00000002,
    D2D1_BITMAP_OPTIONS_CPU_READ = 0x00000004,
    D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE = 0x00000008,
};
inline constexpr D2D1_BITMAP_OPTIONS operator|(D2D1_BITMAP_OPTIONS a, D2D1_BITMAP_OPTIONS b) {
    return static_cast<D2D1_BITMAP_OPTIONS>(static_cast<int>(a) | static_cast<int>(b));
}

struct D2D1_BITMAP_PROPERTIES {
    D2D1_PIXEL_FORMAT pixelFormat;
    FLOAT dpiX;
    FLOAT dpiY;
};

struct D2D1_BITMAP_PROPERTIES1 {
    D2D1_PIXEL_FORMAT pixelFormat;
    FLOAT dpiX;
    FLOAT dpiY;
    D2D1_BITMAP_OPTIONS bitmapOptions;
    void* colorContext;
};

enum D2D1_ANTIALIAS_MODE {
    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE = 0,
    D2D1_ANTIALIAS_MODE_ALIASED = 1,
};

enum D2D1_TEXT_ANTIALIAS_MODE {
    D2D1_TEXT_ANTIALIAS_MODE_DEFAULT = 0,
    D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE = 1,
    D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE = 2,
    D2D1_TEXT_ANTIALIAS_MODE_ALIASED = 3,
};

enum D2D1_BITMAP_INTERPOLATION_MODE {
    D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR = 0,
    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR = 1,
};

enum D2D1_INTERPOLATION_MODE {
    D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR = 0,
    D2D1_INTERPOLATION_MODE_LINEAR = 1,
    D2D1_INTERPOLATION_MODE_CUBIC = 2,
    D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR = 3,
    D2D1_INTERPOLATION_MODE_ANISOTROPIC = 4,
    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC = 5,
};

enum D2D1_COMPOSITE_MODE {
    D2D1_COMPOSITE_MODE_SOURCE_OVER = 0,
    D2D1_COMPOSITE_MODE_DESTINATION_OVER = 1,
    D2D1_COMPOSITE_MODE_SOURCE_IN = 2,
    D2D1_COMPOSITE_MODE_DESTINATION_IN = 3,
    D2D1_COMPOSITE_MODE_SOURCE_OUT = 4,
    D2D1_COMPOSITE_MODE_DESTINATION_OUT = 5,
    D2D1_COMPOSITE_MODE_SOURCE_ATOP = 6,
    D2D1_COMPOSITE_MODE_DESTINATION_ATOP = 7,
    D2D1_COMPOSITE_MODE_XOR = 8,
    D2D1_COMPOSITE_MODE_PLUS = 9,
    D2D1_COMPOSITE_MODE_SOURCE_COPY = 10,
    D2D1_COMPOSITE_MODE_BOUNDED_SOURCE_COPY = 11,
    D2D1_COMPOSITE_MODE_MASK_INVERT = 12,
};

enum D2D1_EXTEND_MODE {
    D2D1_EXTEND_MODE_CLAMP = 0,
    D2D1_EXTEND_MODE_WRAP = 1,
    D2D1_EXTEND_MODE_MIRROR = 2,
};

enum D2D1_GAMMA {
    D2D1_GAMMA_2_2 = 0,
    D2D1_GAMMA_1_0 = 1,
};

struct D2D1_GRADIENT_STOP {
    FLOAT position;
    D2D1_COLOR_F color;
};

struct D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES {
    D2D1_POINT_2F startPoint;
    D2D1_POINT_2F endPoint;
};

struct D2D1_BITMAP_BRUSH_PROPERTIES {
    D2D1_EXTEND_MODE extendModeX;
    D2D1_EXTEND_MODE extendModeY;
    D2D1_BITMAP_INTERPOLATION_MODE interpolationMode;
};

struct D2D1_BRUSH_PROPERTIES {
    FLOAT opacity;
    D2D1_MATRIX_3X2_F transform;
};

enum D2D1_LAYER_OPTIONS {
    D2D1_LAYER_OPTIONS_NONE = 0x00000000,
    D2D1_LAYER_OPTIONS_INITIALIZE_FOR_CLEARTYPE = 0x00000001,
};

enum D2D1_LAYER_OPTIONS1 {
    D2D1_LAYER_OPTIONS1_NONE = 0,
    D2D1_LAYER_OPTIONS1_INITIALIZE_FROM_BACKGROUND = 1,
    D2D1_LAYER_OPTIONS1_IGNORE_ALPHA = 2,
};

struct ID2D1Geometry;
struct ID2D1Brush;

struct D2D1_LAYER_PARAMETERS {
    D2D1_RECT_F contentBounds;
    ID2D1Geometry* geometricMask;
    D2D1_ANTIALIAS_MODE maskAntialiasMode;
    D2D1_MATRIX_3X2_F maskTransform;
    FLOAT opacity;
    ID2D1Brush* opacityBrush;
    D2D1_LAYER_OPTIONS layerOptions;
};

struct D2D1_LAYER_PARAMETERS1 {
    D2D1_RECT_F contentBounds;
    ID2D1Geometry* geometricMask;
    D2D1_ANTIALIAS_MODE maskAntialiasMode;
    D2D1_MATRIX_3X2_F maskTransform;
    FLOAT opacity;
    ID2D1Brush* opacityBrush;
    D2D1_LAYER_OPTIONS1 layerOptions;
};

enum D2D1_FIGURE_BEGIN {
    D2D1_FIGURE_BEGIN_FILLED = 0,
    D2D1_FIGURE_BEGIN_HOLLOW = 1,
};

enum D2D1_FIGURE_END {
    D2D1_FIGURE_END_OPEN = 0,
    D2D1_FIGURE_END_CLOSED = 1,
};

enum D2D1_FILL_MODE {
    D2D1_FILL_MODE_ALTERNATE = 0,
    D2D1_FILL_MODE_WINDING = 1,
};

enum D2D1_SWEEP_DIRECTION {
    D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE = 0,
    D2D1_SWEEP_DIRECTION_CLOCKWISE = 1,
};

enum D2D1_ARC_SIZE {
    D2D1_ARC_SIZE_SMALL = 0,
    D2D1_ARC_SIZE_LARGE = 1,
};

struct D2D1_ARC_SEGMENT {
    D2D1_POINT_2F point;
    D2D1_SIZE_F size;
    FLOAT rotationAngle;
    D2D1_SWEEP_DIRECTION sweepDirection;
    D2D1_ARC_SIZE arcSize;
};

struct D2D1_BEZIER_SEGMENT {
    D2D1_POINT_2F point1;
    D2D1_POINT_2F point2;
    D2D1_POINT_2F point3;
};

struct D2D1_QUADRATIC_BEZIER_SEGMENT {
    D2D1_POINT_2F point1;
    D2D1_POINT_2F point2;
};

enum D2D1_COMBINE_MODE {
    D2D1_COMBINE_MODE_UNION = 0,
    D2D1_COMBINE_MODE_INTERSECT = 1,
    D2D1_COMBINE_MODE_XOR = 2,
    D2D1_COMBINE_MODE_EXCLUDE = 3,
};

enum D2D1_DEVICE_CONTEXT_OPTIONS {
    D2D1_DEVICE_CONTEXT_OPTIONS_NONE = 0,
    D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS = 1,
};

enum D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS {
    D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE = 0x00000000,
    D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_GDI_COMPATIBLE = 0x00000001,
};

enum D2D1_CAP_STYLE {
    D2D1_CAP_STYLE_FLAT = 0,
    D2D1_CAP_STYLE_SQUARE = 1,
    D2D1_CAP_STYLE_ROUND = 2,
    D2D1_CAP_STYLE_TRIANGLE = 3,
};

enum D2D1_LINE_JOIN {
    D2D1_LINE_JOIN_MITER = 0,
    D2D1_LINE_JOIN_BEVEL = 1,
    D2D1_LINE_JOIN_ROUND = 2,
    D2D1_LINE_JOIN_MITER_OR_BEVEL = 3,
};

enum D2D1_DASH_STYLE {
    D2D1_DASH_STYLE_SOLID = 0,
    D2D1_DASH_STYLE_DASH = 1,
    D2D1_DASH_STYLE_DOT = 2,
    D2D1_DASH_STYLE_DASH_DOT = 3,
    D2D1_DASH_STYLE_DASH_DOT_DOT = 4,
    D2D1_DASH_STYLE_CUSTOM = 5,
};

struct D2D1_STROKE_STYLE_PROPERTIES {
    D2D1_CAP_STYLE startCap;
    D2D1_CAP_STYLE endCap;
    D2D1_CAP_STYLE dashCap;
    D2D1_LINE_JOIN lineJoin;
    FLOAT miterLimit;
    D2D1_DASH_STYLE dashStyle;
    FLOAT dashOffset;
};

enum D2D1_DRAW_TEXT_OPTIONS {
    D2D1_DRAW_TEXT_OPTIONS_NO_SNAP = 0x00000001,
    D2D1_DRAW_TEXT_OPTIONS_CLIP = 0x00000002,
    D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT = 0x00000004,
    D2D1_DRAW_TEXT_OPTIONS_DISABLE_COLOR_BITMAP_SNAPPING = 0x00000008,
    D2D1_DRAW_TEXT_OPTIONS_NONE = 0x00000000,
};
inline constexpr D2D1_DRAW_TEXT_OPTIONS operator|(D2D1_DRAW_TEXT_OPTIONS a, D2D1_DRAW_TEXT_OPTIONS b) {
    return static_cast<D2D1_DRAW_TEXT_OPTIONS>(static_cast<int>(a) | static_cast<int>(b));
}

namespace D2D1 {

class ColorF : public D2D1_COLOR_F {
public:
    enum Enum {
        Black = 0x000000,
        White = 0xFFFFFF,
        Red = 0xFF0000,
        Green = 0x00FF00,
        Blue = 0x0000FF,
    };
    constexpr ColorF() noexcept : D2D1_COLOR_F{0.f, 0.f, 0.f, 1.f} {}
    constexpr ColorF(FLOAT r_, FLOAT g_, FLOAT b_, FLOAT a_ = 1.f) noexcept
        : D2D1_COLOR_F{r_, g_, b_, a_} {}
    constexpr ColorF(Enum knownColor, FLOAT a_ = 1.f) noexcept
        : D2D1_COLOR_F{
              static_cast<FLOAT>((static_cast<UINT32>(knownColor) >> 16) & 0xFF) / 255.f,
              static_cast<FLOAT>((static_cast<UINT32>(knownColor) >> 8) & 0xFF) / 255.f,
              static_cast<FLOAT>(static_cast<UINT32>(knownColor) & 0xFF) / 255.f,
              a_} {}
    constexpr ColorF(UINT32 rgb, FLOAT a_ = 1.f) noexcept
        : D2D1_COLOR_F{
              static_cast<FLOAT>((rgb >> 16) & 0xFF) / 255.f,
              static_cast<FLOAT>((rgb >> 8) & 0xFF) / 255.f,
              static_cast<FLOAT>(rgb & 0xFF) / 255.f,
              a_} {}
};

inline constexpr D2D1_STROKE_STYLE_PROPERTIES StrokeStyleProperties(
    D2D1_CAP_STYLE startCap = D2D1_CAP_STYLE_FLAT,
    D2D1_CAP_STYLE endCap = D2D1_CAP_STYLE_FLAT,
    D2D1_CAP_STYLE dashCap = D2D1_CAP_STYLE_FLAT,
    D2D1_LINE_JOIN lineJoin = D2D1_LINE_JOIN_MITER,
    FLOAT miterLimit = 10.0f,
    D2D1_DASH_STYLE dashStyle = D2D1_DASH_STYLE_SOLID,
    FLOAT dashOffset = 0.0f) noexcept {
    return D2D1_STROKE_STYLE_PROPERTIES{startCap, endCap, dashCap, lineJoin, miterLimit, dashStyle, dashOffset};
}

inline constexpr D2D1_POINT_2F Point2F(FLOAT x = 0.f, FLOAT y = 0.f) noexcept {
    return D2D1_POINT_2F{x, y};
}

inline constexpr D2D1_SIZE_F SizeF(FLOAT width = 0.f, FLOAT height = 0.f) noexcept {
    return D2D1_SIZE_F{width, height};
}

inline constexpr D2D1_SIZE_U SizeU(UINT32 width = 0, UINT32 height = 0) noexcept {
    return D2D1_SIZE_U{width, height};
}

inline constexpr D2D1_RECT_F RectF(FLOAT left = 0.f, FLOAT top = 0.f, FLOAT right = 0.f, FLOAT bottom = 0.f) noexcept {
    return D2D1_RECT_F{left, top, right, bottom};
}

inline constexpr D2D1_RECT_U RectU(UINT32 left = 0, UINT32 top = 0, UINT32 right = 0, UINT32 bottom = 0) noexcept {
    return D2D1_RECT_U{left, top, right, bottom};
}

inline constexpr D2D1_ROUNDED_RECT RoundedRect(const D2D1_RECT_F& rect, FLOAT radiusX, FLOAT radiusY) noexcept {
    return D2D1_ROUNDED_RECT{rect, radiusX, radiusY};
}

inline constexpr D2D1_ELLIPSE Ellipse(const D2D1_POINT_2F& center, FLOAT radiusX, FLOAT radiusY) noexcept {
    return D2D1_ELLIPSE{center, radiusX, radiusY};
}

inline constexpr D2D1_PIXEL_FORMAT PixelFormat(
    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN,
    D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_UNKNOWN) noexcept {
    return D2D1_PIXEL_FORMAT{dxgiFormat, alphaMode};
}

inline constexpr D2D1_RENDER_TARGET_PROPERTIES RenderTargetProperties(
    D2D1_RENDER_TARGET_TYPE type = D2D1_RENDER_TARGET_TYPE_DEFAULT,
    const D2D1_PIXEL_FORMAT& pixelFormat = PixelFormat(),
    FLOAT dpiX = 0.0f,
    FLOAT dpiY = 0.0f,
    D2D1_RENDER_TARGET_USAGE usage = D2D1_RENDER_TARGET_USAGE_NONE,
    D2D1_FEATURE_LEVEL minLevel = D2D1_FEATURE_LEVEL_DEFAULT) noexcept {
    return D2D1_RENDER_TARGET_PROPERTIES{type, pixelFormat, dpiX, dpiY, usage, minLevel};
}

inline constexpr D2D1_BITMAP_PROPERTIES BitmapProperties(
    const D2D1_PIXEL_FORMAT& pixelFormat = PixelFormat(),
    FLOAT dpiX = 96.0f,
    FLOAT dpiY = 96.0f) noexcept {
    return D2D1_BITMAP_PROPERTIES{pixelFormat, dpiX, dpiY};
}

inline constexpr D2D1_BITMAP_PROPERTIES1 BitmapProperties1(
    D2D1_BITMAP_OPTIONS bitmapOptions = D2D1_BITMAP_OPTIONS_NONE,
    const D2D1_PIXEL_FORMAT& pixelFormat = PixelFormat(),
    FLOAT dpiX = 96.0f,
    FLOAT dpiY = 96.0f,
    void* colorContext = nullptr) noexcept {
    return D2D1_BITMAP_PROPERTIES1{pixelFormat, dpiX, dpiY, bitmapOptions, colorContext};
}

inline constexpr D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES LinearGradientBrushProperties(
    const D2D1_POINT_2F& startPoint,
    const D2D1_POINT_2F& endPoint) noexcept {
    return D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES{startPoint, endPoint};
}

inline constexpr D2D1_BITMAP_BRUSH_PROPERTIES BitmapBrushProperties(
    D2D1_EXTEND_MODE extendModeX = D2D1_EXTEND_MODE_CLAMP,
    D2D1_EXTEND_MODE extendModeY = D2D1_EXTEND_MODE_CLAMP,
    D2D1_BITMAP_INTERPOLATION_MODE interpolationMode = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR) noexcept {
    return D2D1_BITMAP_BRUSH_PROPERTIES{extendModeX, extendModeY, interpolationMode};
}

inline constexpr D2D1_ARC_SEGMENT ArcSegment(
    const D2D1_POINT_2F& point,
    const D2D1_SIZE_F& size,
    FLOAT rotationAngle,
    D2D1_SWEEP_DIRECTION sweepDirection,
    D2D1_ARC_SIZE arcSize) noexcept {
    return D2D1_ARC_SEGMENT{point, size, rotationAngle, sweepDirection, arcSize};
}

inline constexpr D2D1_BEZIER_SEGMENT BezierSegment(
    const D2D1_POINT_2F& point1,
    const D2D1_POINT_2F& point2,
    const D2D1_POINT_2F& point3) noexcept {
    return D2D1_BEZIER_SEGMENT{point1, point2, point3};
}

inline constexpr D2D1_QUADRATIC_BEZIER_SEGMENT QuadraticBezierSegment(
    const D2D1_POINT_2F& point1,
    const D2D1_POINT_2F& point2) noexcept {
    return D2D1_QUADRATIC_BEZIER_SEGMENT{point1, point2};
}

inline constexpr D2D1_RECT_F InfiniteRect() noexcept {
    return D2D1_RECT_F{-1e6f, -1e6f, 1e6f, 1e6f};
}

class Matrix3x2F : public D2D1_MATRIX_3X2_F {
public:
    constexpr Matrix3x2F(FLOAT _11 = 1.f, FLOAT _12 = 0.f, FLOAT _21 = 0.f, FLOAT _22 = 1.f, FLOAT _31 = 0.f, FLOAT _32 = 0.f) noexcept
        : D2D1_MATRIX_3X2_F{{{_11, _12, _21, _22, _31, _32}}} {}
    static constexpr Matrix3x2F Identity() noexcept {
        return Matrix3x2F(1.f, 0.f, 0.f, 1.f, 0.f, 0.f);
    }
    static constexpr Matrix3x2F Translation(FLOAT x, FLOAT y) noexcept {
        return Matrix3x2F(1.f, 0.f, 0.f, 1.f, x, y);
    }
    static constexpr Matrix3x2F Translation(D2D1_SIZE_F size) noexcept {
        return Matrix3x2F(1.f, 0.f, 0.f, 1.f, size.width, size.height);
    }
    static constexpr Matrix3x2F Scale(FLOAT x, FLOAT y, D2D1_POINT_2F center = D2D1_POINT_2F{0.f, 0.f}) noexcept {
        return Matrix3x2F(x, 0.f, 0.f, y, center.x - x * center.x, center.y - y * center.y);
    }
    static constexpr Matrix3x2F Scale(D2D1_SIZE_F size, D2D1_POINT_2F center = D2D1_POINT_2F{0.f, 0.f}) noexcept {
        return Scale(size.width, size.height, center);
    }
    static inline Matrix3x2F Rotation(FLOAT angle, D2D1_POINT_2F center = D2D1_POINT_2F{0.f, 0.f}) noexcept {
        FLOAT rad = angle * 3.14159265358979323846f / 180.0f;
        FLOAT s = std::sin(rad);
        FLOAT c = std::cos(rad);
        return Matrix3x2F(c, s, -s, c, center.x * (1.0f - c) + center.y * s, center.y * (1.0f - c) - center.x * s);
    }
    Matrix3x2F operator*(const Matrix3x2F& b) const noexcept {
        return Matrix3x2F(
            m11 * b.m11 + m12 * b.m21,
            m11 * b.m12 + m12 * b.m22,
            m21 * b.m11 + m22 * b.m21,
            m21 * b.m12 + m22 * b.m22,
            dx * b.m11 + dy * b.m21 + b.dx,
            dx * b.m12 + dy * b.m22 + b.dy);
    }
};

inline constexpr D2D1_LAYER_PARAMETERS LayerParameters(
    const D2D1_RECT_F& contentBounds = InfiniteRect(),
    ID2D1Geometry* geometricMask = nullptr,
    D2D1_ANTIALIAS_MODE maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
    D2D1_MATRIX_3X2_F maskTransform = Matrix3x2F::Identity(),
    FLOAT opacity = 1.0f,
    ID2D1Brush* opacityBrush = nullptr,
    D2D1_LAYER_OPTIONS layerOptions = D2D1_LAYER_OPTIONS_NONE) noexcept {
    return D2D1_LAYER_PARAMETERS{contentBounds, geometricMask, maskAntialiasMode, maskTransform, opacity, opacityBrush, layerOptions};
}

inline constexpr D2D1_LAYER_PARAMETERS1 LayerParameters1(
    const D2D1_RECT_F& contentBounds = InfiniteRect(),
    ID2D1Geometry* geometricMask = nullptr,
    D2D1_ANTIALIAS_MODE maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
    D2D1_MATRIX_3X2_F maskTransform = Matrix3x2F::Identity(),
    FLOAT opacity = 1.0f,
    ID2D1Brush* opacityBrush = nullptr,
    D2D1_LAYER_OPTIONS1 layerOptions = D2D1_LAYER_OPTIONS1_NONE) noexcept {
    return D2D1_LAYER_PARAMETERS1{contentBounds, geometricMask, maskAntialiasMode, maskTransform, opacity, opacityBrush, layerOptions};
}

} // namespace D2D1

// ============================================================================
// DirectWrite Types & Interfaces
// ============================================================================

enum DWRITE_FACTORY_TYPE {
    DWRITE_FACTORY_TYPE_SHARED = 0,
    DWRITE_FACTORY_TYPE_ISOLATED = 1,
};

enum DWRITE_FONT_WEIGHT {
    DWRITE_FONT_WEIGHT_THIN = 100,
    DWRITE_FONT_WEIGHT_EXTRA_LIGHT = 200,
    DWRITE_FONT_WEIGHT_LIGHT = 300,
    DWRITE_FONT_WEIGHT_SEMI_LIGHT = 350,
    DWRITE_FONT_WEIGHT_NORMAL = 400,
    DWRITE_FONT_WEIGHT_REGULAR = 400,
    DWRITE_FONT_WEIGHT_MEDIUM = 500,
    DWRITE_FONT_WEIGHT_SEMI_BOLD = 600,
    DWRITE_FONT_WEIGHT_BOLD = 700,
    DWRITE_FONT_WEIGHT_EXTRA_BOLD = 800,
    DWRITE_FONT_WEIGHT_BLACK = 900,
};

enum DWRITE_FONT_STYLE {
    DWRITE_FONT_STYLE_NORMAL = 0,
    DWRITE_FONT_STYLE_OBLIQUE = 1,
    DWRITE_FONT_STYLE_ITALIC = 2,
};

enum DWRITE_FONT_STRETCH {
    DWRITE_FONT_STRETCH_UNDEFINED = 0,
    DWRITE_FONT_STRETCH_NORMAL = 5,
};

enum DWRITE_TEXT_ALIGNMENT {
    DWRITE_TEXT_ALIGNMENT_LEADING = 0,
    DWRITE_TEXT_ALIGNMENT_TRAILING = 1,
    DWRITE_TEXT_ALIGNMENT_CENTER = 2,
    DWRITE_TEXT_ALIGNMENT_JUSTIFIED = 3,
};

enum DWRITE_PARAGRAPH_ALIGNMENT {
    DWRITE_PARAGRAPH_ALIGNMENT_NEAR = 0,
    DWRITE_PARAGRAPH_ALIGNMENT_FAR = 1,
    DWRITE_PARAGRAPH_ALIGNMENT_CENTER = 2,
};

enum DWRITE_WORD_WRAPPING {
    DWRITE_WORD_WRAPPING_WRAP = 0,
    DWRITE_WORD_WRAPPING_NO_WRAP = 1,
    DWRITE_WORD_WRAPPING_EMERGENCY_BREAK = 2,
    DWRITE_WORD_WRAPPING_WHOLE_WORD = 3,
    DWRITE_WORD_WRAPPING_CHARACTER = 4,
};

enum DWRITE_TRIMMING_GRANULARITY {
    DWRITE_TRIMMING_GRANULARITY_NONE = 0,
    DWRITE_TRIMMING_GRANULARITY_CHARACTER = 1,
    DWRITE_TRIMMING_GRANULARITY_WORD = 2,
};

struct DWRITE_TRIMMING {
    DWRITE_TRIMMING_GRANULARITY granularity;
    UINT32 delimiter;
    UINT32 delimiterCount;
};

struct DWRITE_TEXT_RANGE {
    UINT32 startPosition;
    UINT32 length;
};

struct DWRITE_TEXT_METRICS {
    FLOAT left;
    FLOAT top;
    FLOAT width;
    FLOAT widthIncludingTrailingWhitespace;
    FLOAT height;
    FLOAT layoutWidth;
    FLOAT layoutHeight;
    UINT32 maxBidiReorderingDepth;
    UINT32 lineCount;
};

struct DWRITE_HIT_TEST_METRICS {
    UINT32 textPosition;
    UINT32 length;
    FLOAT left;
    FLOAT top;
    FLOAT width;
    FLOAT height;
    UINT32 bidiLevel;
    BOOL isText;
    BOOL isTrimmed;
};

enum DWRITE_MEASURING_MODE {
    DWRITE_MEASURING_MODE_NATURAL = 0,
    DWRITE_MEASURING_MODE_GDI_CLASSIC = 1,
    DWRITE_MEASURING_MODE_GDI_NATURAL = 2,
};

struct IDWriteInlineObject : public IUnknown {};
struct IDWriteFontCollection : public IUnknown {
    virtual HRESULT FindFamilyName(const WCHAR* /*familyName*/, UINT32* index, BOOL* exists) {
        if (index) *index = 0;
        if (exists) *exists = TRUE;
        return S_OK;
    }
};
struct IDWriteFontFile : public IUnknown {};
struct IDWriteFontSet : public IUnknown {};

enum DWRITE_READING_DIRECTION {
    DWRITE_READING_DIRECTION_LEFT_TO_RIGHT = 0,
    DWRITE_READING_DIRECTION_RIGHT_TO_LEFT = 1,
    DWRITE_READING_DIRECTION_TOP_TO_BOTTOM = 2,
    DWRITE_READING_DIRECTION_BOTTOM_TO_TOP = 3,
};

enum DWRITE_FLOW_DIRECTION {
    DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM = 0,
    DWRITE_FLOW_DIRECTION_BOTTOM_TO_TOP = 1,
    DWRITE_FLOW_DIRECTION_LEFT_TO_RIGHT = 2,
    DWRITE_FLOW_DIRECTION_RIGHT_TO_LEFT = 3,
};

enum DWRITE_PIXEL_GEOMETRY {
    DWRITE_PIXEL_GEOMETRY_FLAT = 0,
    DWRITE_PIXEL_GEOMETRY_RGB = 1,
    DWRITE_PIXEL_GEOMETRY_BGR = 2,
};

enum DWRITE_RENDERING_MODE1 {
    DWRITE_RENDERING_MODE1_DEFAULT = 0,
    DWRITE_RENDERING_MODE1_ALIASED = 1,
    DWRITE_RENDERING_MODE1_GDI_CLASSIC = 2,
    DWRITE_RENDERING_MODE1_GDI_NATURAL = 3,
    DWRITE_RENDERING_MODE1_NATURAL = 4,
    DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC = 5,
    DWRITE_RENDERING_MODE1_OUTLINE = 6,
};

enum DWRITE_GRID_FIT_MODE {
    DWRITE_GRID_FIT_MODE_DEFAULT = 0,
    DWRITE_GRID_FIT_MODE_DISABLED = 1,
    DWRITE_GRID_FIT_MODE_ENABLED = 2,
};

struct IDWriteRenderingParams : public IUnknown {
    virtual FLOAT GetGamma() { return 1.8f; }
    virtual FLOAT GetEnhancedContrast() { return 0.5f; }
    virtual FLOAT GetClearTypeLevel() { return 1.0f; }
    virtual DWRITE_PIXEL_GEOMETRY GetPixelGeometry() { return DWRITE_PIXEL_GEOMETRY_RGB; }
};

struct IDWriteRenderingParams1 : public IDWriteRenderingParams {};
struct IDWriteRenderingParams2 : public IDWriteRenderingParams1 {};
struct IDWriteRenderingParams3 : public IDWriteRenderingParams2 {};

struct IDWriteFontSetBuilder1 : public IUnknown {
    virtual HRESULT AddFontFile(IDWriteFontFile* /*fontFile*/) { return S_OK; }
    virtual HRESULT CreateFontSet(IDWriteFontSet** fontSet) {
        if (!fontSet) return E_POINTER;
        *fontSet = new IDWriteFontSet();
        return S_OK;
    }
};

struct IDWriteFontCollection1 : public IDWriteFontCollection {};

struct IDWriteTextFormat : public IUnknown {
    std::wstring m_familyName = L"sans-serif";
    DWRITE_FONT_WEIGHT m_weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE m_style = DWRITE_FONT_STYLE_NORMAL;
    DWRITE_FONT_STRETCH m_stretch = DWRITE_FONT_STRETCH_NORMAL;
    FLOAT m_fontSize = 14.0f;
    DWRITE_TEXT_ALIGNMENT m_textAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
    DWRITE_PARAGRAPH_ALIGNMENT m_paragraphAlignment = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    DWRITE_WORD_WRAPPING m_wordWrapping = DWRITE_WORD_WRAPPING_WRAP;
    DWRITE_TRIMMING m_trimming = {DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
    DWRITE_READING_DIRECTION m_readingDirection = DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    DWRITE_FLOW_DIRECTION m_flowDirection = DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM;

    virtual HRESULT SetTextAlignment(DWRITE_TEXT_ALIGNMENT textAlignment) {
        m_textAlignment = textAlignment;
        return S_OK;
    }
    virtual HRESULT SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment) {
        m_paragraphAlignment = paragraphAlignment;
        return S_OK;
    }
    virtual HRESULT SetWordWrapping(DWRITE_WORD_WRAPPING wordWrapping) {
        m_wordWrapping = wordWrapping;
        return S_OK;
    }
    virtual HRESULT SetTrimming(const DWRITE_TRIMMING* trimmingOptions, IDWriteInlineObject* /*trimmingSign*/) {
        if (trimmingOptions) m_trimming = *trimmingOptions;
        return S_OK;
    }
    virtual HRESULT SetReadingDirection(DWRITE_READING_DIRECTION readingDirection) {
        m_readingDirection = readingDirection;
        return S_OK;
    }
    virtual HRESULT SetFlowDirection(DWRITE_FLOW_DIRECTION flowDirection) {
        m_flowDirection = flowDirection;
        return S_OK;
    }
    virtual DWRITE_READING_DIRECTION GetReadingDirection() { return m_readingDirection; }
    virtual DWRITE_FLOW_DIRECTION GetFlowDirection() { return m_flowDirection; }
    virtual DWRITE_TEXT_ALIGNMENT GetTextAlignment() { return m_textAlignment; }
    virtual DWRITE_PARAGRAPH_ALIGNMENT GetParagraphAlignment() { return m_paragraphAlignment; }
    virtual DWRITE_WORD_WRAPPING GetWordWrapping() { return m_wordWrapping; }
    virtual FLOAT GetFontSize() { return m_fontSize; }
};

struct IDWriteTextLayout : public IDWriteTextFormat {
    std::wstring m_text;
    FLOAT m_maxWidth = 1000.0f;
    FLOAT m_maxHeight = 1000.0f;

    virtual HRESULT SetMaxWidth(FLOAT maxWidth) { m_maxWidth = maxWidth; return S_OK; }
    virtual HRESULT SetMaxHeight(FLOAT maxHeight) { m_maxHeight = maxHeight; return S_OK; }
    virtual HRESULT SetFontWeight(DWRITE_FONT_WEIGHT fontWeight, DWRITE_TEXT_RANGE textRange) {
        m_weight = fontWeight;
        return S_OK;
    }
    virtual HRESULT SetFontStyle(DWRITE_FONT_STYLE fontStyle, DWRITE_TEXT_RANGE textRange) {
        m_style = fontStyle;
        return S_OK;
    }
    virtual HRESULT SetFontSize(FLOAT fontSize, DWRITE_TEXT_RANGE textRange) {
        m_fontSize = fontSize;
        return S_OK;
    }
    virtual HRESULT SetFontFamilyName(const WCHAR* fontFamilyName, DWRITE_TEXT_RANGE textRange) {
        if (fontFamilyName) m_familyName = fontFamilyName;
        return S_OK;
    }
    virtual HRESULT GetMetrics(DWRITE_TEXT_METRICS* textMetrics);
    virtual HRESULT HitTestPoint(FLOAT pointX, FLOAT pointY, BOOL* isTrailingHit, BOOL* isInside, DWRITE_HIT_TEST_METRICS* hitTestMetrics);
    virtual HRESULT HitTestTextPosition(UINT32 textPosition, BOOL isTrailingHit, FLOAT* pointX, FLOAT* pointY, DWRITE_HIT_TEST_METRICS* hitTestMetrics);
};

struct IDWriteFactory : public IUnknown {
    virtual HRESULT CreateTextFormat(
        const WCHAR* fontFamilyName,
        IDWriteFontCollection* fontCollection,
        DWRITE_FONT_WEIGHT fontWeight,
        DWRITE_FONT_STYLE fontStyle,
        DWRITE_FONT_STRETCH fontStretch,
        FLOAT fontSize,
        const WCHAR* localeName,
        IDWriteTextFormat** textFormat) {
        if (!textFormat) return E_POINTER;
        auto* tf = new IDWriteTextFormat();
        if (fontFamilyName) tf->m_familyName = fontFamilyName;
        tf->m_weight = fontWeight;
        tf->m_style = fontStyle;
        tf->m_stretch = fontStretch;
        tf->m_fontSize = fontSize > 0.f ? fontSize : 12.f;
        *textFormat = tf;
        return S_OK;
    }
    virtual HRESULT CreateTextLayout(
        const WCHAR* string,
        UINT32 stringLength,
        IDWriteTextFormat* textFormat,
        FLOAT maxWidth,
        FLOAT maxHeight,
        IDWriteTextLayout** textLayout) {
        if (!textLayout) return E_POINTER;
        auto* tl = new IDWriteTextLayout();
        if (textFormat) {
            tl->m_familyName = textFormat->m_familyName;
            tl->m_weight = textFormat->m_weight;
            tl->m_style = textFormat->m_style;
            tl->m_stretch = textFormat->m_stretch;
            tl->m_fontSize = textFormat->m_fontSize;
            tl->m_textAlignment = textFormat->m_textAlignment;
            tl->m_paragraphAlignment = textFormat->m_paragraphAlignment;
            tl->m_wordWrapping = textFormat->m_wordWrapping;
            tl->m_trimming = textFormat->m_trimming;
        }
        if (string && stringLength > 0) {
            tl->m_text.assign(string, stringLength);
        }
        tl->m_maxWidth = maxWidth;
        tl->m_maxHeight = maxHeight;
        *textLayout = tl;
        return S_OK;
    }
    virtual HRESULT CreateEllipsisTrimmingSign(IDWriteTextFormat* textFormat, IDWriteInlineObject** trimmingSign) {
        if (!trimmingSign) return E_POINTER;
        *trimmingSign = new IDWriteInlineObject();
        return S_OK;
    }
    virtual HRESULT CreateFontFileReference(const WCHAR* filePath, const FILETIME* lastWriteTime, IDWriteFontFile** fontFile) {
        if (!fontFile) return E_POINTER;
        *fontFile = new IDWriteFontFile();
        return S_OK;
    }
    virtual HRESULT CreateMonitorRenderingParams(HMONITOR /*monitor*/, IDWriteRenderingParams** renderingParams) {
        if (!renderingParams) return E_POINTER;
        *renderingParams = new IDWriteRenderingParams3();
        return S_OK;
    }
    virtual HRESULT CreateRenderingParams(IDWriteRenderingParams** renderingParams) {
        if (!renderingParams) return E_POINTER;
        *renderingParams = new IDWriteRenderingParams3();
        return S_OK;
    }
};

struct IDWriteFactory3 : public IDWriteFactory {
    virtual HRESULT CreateCustomRenderingParams(
        FLOAT /*gamma*/, FLOAT /*enhancedContrast*/, FLOAT /*grayscaleEnhancedContrast*/,
        FLOAT /*clearTypeLevel*/, DWRITE_PIXEL_GEOMETRY /*pixelGeometry*/,
        DWRITE_RENDERING_MODE1 /*renderingMode*/, DWRITE_GRID_FIT_MODE /*gridFitMode*/,
        IDWriteRenderingParams3** renderingParams) {
        if (!renderingParams) return E_POINTER;
        *renderingParams = new IDWriteRenderingParams3();
        return S_OK;
    }
};

struct IDWriteFactory5 : public IDWriteFactory3 {
    virtual HRESULT CreateFontSetBuilder(IDWriteFontSetBuilder1** fontSetBuilder) {
        if (!fontSetBuilder) return E_POINTER;
        *fontSetBuilder = new IDWriteFontSetBuilder1();
        return S_OK;
    }
    virtual HRESULT CreateFontCollectionFromFontSet(IUnknown* fontSet, IDWriteFontCollection1** fontCollection) {
        if (!fontCollection) return E_POINTER;
        *fontCollection = new IDWriteFontCollection1();
        return S_OK;
    }
};

inline HRESULT DWriteCreateFactory(DWRITE_FACTORY_TYPE factoryType, REFIID iid, IUnknown** factory) {
    if (!factory) return E_POINTER;
    *factory = new IDWriteFactory5();
    return S_OK;
}

// ============================================================================
// Direct2D Resource & RenderTarget Interfaces
// ============================================================================

struct ID2D1Factory;

struct ID2D1Resource : public IUnknown {
    virtual void GetFactory(ID2D1Factory** factory) const;
};
struct ID2D1Image : public ID2D1Resource {};

struct ID2D1Bitmap : public ID2D1Image {
    UINT32 m_width = 1;
    UINT32 m_height = 1;
    FLOAT m_dpiX = 96.0f;
    FLOAT m_dpiY = 96.0f;
    std::vector<uint8_t> m_pixels; // BGRA premultiplied

    ID2D1Bitmap(UINT32 w = 1, UINT32 h = 1, const void* srcData = nullptr, UINT32 pitch = 0)
        : m_width(w ? w : 1), m_height(h ? h : 1) {
        m_pixels.resize((size_t)m_width * m_height * 4, 0);
        if (srcData && pitch > 0) {
            const uint8_t* src = static_cast<const uint8_t*>(srcData);
            size_t rowBytes = (size_t)m_width * 4;
            for (UINT32 y = 0; y < m_height; ++y) {
                std::memcpy(m_pixels.data() + (size_t)y * rowBytes, src + (size_t)y * pitch, rowBytes);
            }
        }
    }
    virtual D2D1_SIZE_F GetSize() const {
        return D2D1::SizeF((FLOAT)m_width * 96.0f / (m_dpiX > 0.f ? m_dpiX : 96.0f),
                           (FLOAT)m_height * 96.0f / (m_dpiY > 0.f ? m_dpiY : 96.0f));
    }
    virtual D2D1_SIZE_U GetPixelSize() const {
        return D2D1::SizeU(m_width, m_height);
    }
    virtual HRESULT CopyFromMemory(const D2D1_RECT_U* dstRect, const void* srcData, UINT32 pitch) {
        if (!srcData || pitch == 0) return E_INVALIDARG;
        UINT32 left = dstRect ? dstRect->left : 0;
        UINT32 top = dstRect ? dstRect->top : 0;
        UINT32 right = dstRect ? dstRect->right : m_width;
        UINT32 bottom = dstRect ? dstRect->bottom : m_height;
        if (right > m_width) right = m_width;
        if (bottom > m_height) bottom = m_height;
        if (left >= right || top >= bottom) return S_OK;
        const uint8_t* src = static_cast<const uint8_t*>(srcData);
        size_t copyW = (size_t)(right - left) * 4;
        for (UINT32 y = top; y < bottom; ++y) {
            std::memcpy(m_pixels.data() + ((size_t)y * m_width + left) * 4,
                        src + (size_t)(y - top) * pitch,
                        copyW);
        }
        return S_OK;
    }
    virtual HRESULT CopyFromBitmap(const D2D1_POINT_2F* destPoint, ID2D1Bitmap* bitmap, const D2D1_RECT_U* srcRect) {
        if (!bitmap) return E_INVALIDARG;
        if (m_width == bitmap->m_width && m_height == bitmap->m_height && !destPoint && !srcRect) {
            m_pixels = bitmap->m_pixels;
        }
        return S_OK;
    }
};

struct ID2D1Bitmap1 : public ID2D1Bitmap {
    using ID2D1Bitmap::ID2D1Bitmap;
};

struct ID2D1GradientStopCollection : public ID2D1Resource {
    std::vector<D2D1_GRADIENT_STOP> m_stops;
};

struct ID2D1Brush : public ID2D1Resource {
    FLOAT m_opacity = 1.0f;
    D2D1_MATRIX_3X2_F m_transform = D2D1::Matrix3x2F::Identity();
    virtual void SetOpacity(FLOAT opacity) { m_opacity = opacity; }
    virtual FLOAT GetOpacity() const { return m_opacity; }
    virtual void SetTransform(const D2D1_MATRIX_3X2_F& transform) { m_transform = transform; }
    virtual void SetTransform(const D2D1_MATRIX_3X2_F* transform) { if (transform) m_transform = *transform; }
};

struct ID2D1SolidColorBrush : public ID2D1Brush {
    D2D1_COLOR_F m_color{1.f, 1.f, 1.f, 1.f};
    ID2D1SolidColorBrush(const D2D1_COLOR_F& c = {1.f, 1.f, 1.f, 1.f}) : m_color(c) {}
    virtual void SetColor(const D2D1_COLOR_F& color) { m_color = color; }
    virtual void SetColor(const D2D1_COLOR_F* color) { if (color) m_color = *color; }
    virtual D2D1_COLOR_F GetColor() const { return m_color; }
};

struct ID2D1LinearGradientBrush : public ID2D1Brush {
    D2D1_POINT_2F m_start{0.f, 0.f};
    D2D1_POINT_2F m_end{0.f, 100.f};
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> m_stops;
    virtual void SetStartPoint(D2D1_POINT_2F startPoint) { m_start = startPoint; }
    virtual void SetEndPoint(D2D1_POINT_2F endPoint) { m_end = endPoint; }
};

struct ID2D1BitmapBrush : public ID2D1Brush {
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_bitmap;
    D2D1_BITMAP_BRUSH_PROPERTIES m_props{};
    virtual void SetBitmap(ID2D1Bitmap* bitmap) { m_bitmap = bitmap; }
};

struct ID2D1StrokeStyle : public ID2D1Resource {};
struct ID2D1Layer : public ID2D1Resource {};

struct LinuxPathCmd {
    enum Type { Begin, Line, Arc, Bezier, End } type;
    D2D1_POINT_2F p1{};
    D2D1_POINT_2F p2{};
    D2D1_POINT_2F p3{};
    D2D1_ARC_SEGMENT arc{};
    D2D1_FIGURE_BEGIN beginType = D2D1_FIGURE_BEGIN_FILLED;
    D2D1_FIGURE_END endType = D2D1_FIGURE_END_CLOSED;
};

struct ID2D1SimplifiedGeometrySink : public IUnknown {
    std::vector<LinuxPathCmd>* m_cmds = nullptr;
    virtual void SetFillMode(D2D1_FILL_MODE fillMode) {}
    virtual void BeginFigure(D2D1_POINT_2F startPoint, D2D1_FIGURE_BEGIN figureBegin) {
        if (m_cmds) {
            LinuxPathCmd c{};
            c.type = LinuxPathCmd::Begin;
            c.p1 = startPoint;
            c.beginType = figureBegin;
            m_cmds->push_back(c);
        }
    }
    virtual void AddLines(const D2D1_POINT_2F* points, UINT32 pointsCount) {
        if (m_cmds && points) {
            for (UINT32 i = 0; i < pointsCount; ++i) {
                LinuxPathCmd c{};
                c.type = LinuxPathCmd::Line;
                c.p1 = points[i];
                m_cmds->push_back(c);
            }
        }
    }
    virtual void AddBeziers(const D2D1_BEZIER_SEGMENT* beziers, UINT32 beziersCount) {
        if (m_cmds && beziers) {
            for (UINT32 i = 0; i < beziersCount; ++i) {
                LinuxPathCmd c{};
                c.type = LinuxPathCmd::Bezier;
                c.p1 = beziers[i].point1;
                c.p2 = beziers[i].point2;
                c.p3 = beziers[i].point3;
                m_cmds->push_back(c);
            }
        }
    }
    virtual void EndFigure(D2D1_FIGURE_END figureEnd) {
        if (m_cmds) {
            LinuxPathCmd c{};
            c.type = LinuxPathCmd::End;
            c.endType = figureEnd;
            m_cmds->push_back(c);
        }
    }
    virtual HRESULT Close() { return S_OK; }
};

struct ID2D1GeometrySink : public ID2D1SimplifiedGeometrySink {
    virtual void AddLine(D2D1_POINT_2F point) {
        AddLines(&point, 1);
    }
    virtual void AddBezier(const D2D1_BEZIER_SEGMENT& bezier) {
        AddBeziers(&bezier, 1);
    }
    virtual void AddBezier(const D2D1_BEZIER_SEGMENT* bezier) {
        if (bezier) AddBeziers(bezier, 1);
    }
    virtual void AddQuadraticBezier(const D2D1_QUADRATIC_BEZIER_SEGMENT& bezier) {
        if (m_cmds) {
            LinuxPathCmd c{};
            c.type = LinuxPathCmd::Bezier;
            c.p1 = bezier.point1;
            c.p2 = bezier.point1;
            c.p3 = bezier.point2;
            m_cmds->push_back(c);
        }
    }
    virtual void AddQuadraticBezier(const D2D1_QUADRATIC_BEZIER_SEGMENT* bezier) {
        if (bezier) AddQuadraticBezier(*bezier);
    }
    virtual void AddQuadraticBeziers(const D2D1_QUADRATIC_BEZIER_SEGMENT* beziers, UINT32 beziersCount) {
        if (beziers) {
            for (UINT32 i = 0; i < beziersCount; ++i) AddQuadraticBezier(beziers[i]);
        }
    }
    virtual void AddArc(const D2D1_ARC_SEGMENT& arc) {
        if (m_cmds) {
            LinuxPathCmd c{};
            c.type = LinuxPathCmd::Arc;
            c.arc = arc;
            m_cmds->push_back(c);
        }
    }
    virtual void AddArc(const D2D1_ARC_SEGMENT* arc) {
        if (arc) AddArc(*arc);
    }
};

struct ID2D1Geometry : public ID2D1Resource {
    std::vector<LinuxPathCmd> m_cmds;
    D2D1_ROUNDED_RECT m_roundedRect{};
    bool m_isRoundedRect = false;

    virtual HRESULT CombineWithGeometry(
        ID2D1Geometry* inputGeometry,
        D2D1_COMBINE_MODE combineMode,
        const D2D1_MATRIX_3X2_F* inputGeometryTransform,
        ID2D1SimplifiedGeometrySink* geometrySink) {
        if (!geometrySink) return E_POINTER;
        if (geometrySink->m_cmds) {
            for (const auto& c : m_cmds) geometrySink->m_cmds->push_back(c);
            if (inputGeometry) {
                for (const auto& c : inputGeometry->m_cmds) geometrySink->m_cmds->push_back(c);
            }
        }
        return S_OK;
    }
    virtual HRESULT CombineWithGeometry(
        ID2D1Geometry* inputGeometry,
        D2D1_COMBINE_MODE combineMode,
        const D2D1_MATRIX_3X2_F& inputGeometryTransform,
        ID2D1SimplifiedGeometrySink* geometrySink) {
        return CombineWithGeometry(inputGeometry, combineMode, &inputGeometryTransform, geometrySink);
    }
};

struct ID2D1RoundedRectangleGeometry : public ID2D1Geometry {
    ID2D1RoundedRectangleGeometry(const D2D1_ROUNDED_RECT& rr = {}) {
        m_roundedRect = rr;
        m_isRoundedRect = true;
    }
};

struct ID2D1PathGeometry : public ID2D1Geometry {
    virtual HRESULT Open(ID2D1GeometrySink** geometrySink) {
        if (!geometrySink) return E_POINTER;
        auto* sink = new ID2D1GeometrySink();
        sink->m_cmds = &m_cmds;
        *geometrySink = sink;
        return S_OK;
    }
};

// Effect stubs (for GaussianBlur / Shadow if ever referenced)
enum D2D1_GAUSSIANBLUR_PROP {
    D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION = 0,
    D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION = 1,
    D2D1_GAUSSIANBLUR_PROP_BORDER_MODE = 2,
};
inline constexpr GUID CLSID_D2D1GaussianBlur = {0x1feb6d69, 0x2fe6, 0x4ac9, {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};
inline constexpr GUID CLSID_D2D1Shadow = {0xc67ea361, 0x1863, 0x4e69, {0x89, 0xdb, 0x69, 0x5d, 0x3e, 0x9a, 0x5b, 0x6b}};

struct ID2D1Effect : public ID2D1Image {
    Microsoft::WRL::ComPtr<ID2D1Image> m_input;
    virtual void SetInput(UINT32 index, ID2D1Image* input, BOOL invalidate = TRUE) { m_input = input; }
    virtual HRESULT SetValue(UINT32 index, FLOAT val) { return S_OK; }
    template <typename T>
    HRESULT SetValue(UINT32 index, T val) { return S_OK; }
    virtual void GetOutput(ID2D1Image** outputImage) {
        if (!outputImage) return;
        *outputImage = m_input ? m_input.Get() : static_cast<ID2D1Image*>(this);
        (*outputImage)->AddRef();
    }
};

struct IWICBitmapSource;
struct ID2D1BitmapRenderTarget;

struct ID2D1RenderTarget : public ID2D1Resource {
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_targetBitmap;
    FLOAT m_dpiX = 96.0f;
    FLOAT m_dpiY = 96.0f;
    D2D1_MATRIX_3X2_F m_transform = D2D1::Matrix3x2F::Identity();
    D2D1_ANTIALIAS_MODE m_antialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
    D2D1_TEXT_ANTIALIAS_MODE m_textAntialiasMode = D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE;
    std::vector<D2D1_RECT_F> m_clipStack;
    std::vector<FLOAT> m_layerOpacityStack;
    HDC m_boundDC = nullptr;
    RECT m_boundRect{0, 0, 1280, 720};

    ID2D1RenderTarget();
    virtual ~ID2D1RenderTarget();

    virtual void BeginDraw();
    virtual HRESULT EndDraw(UINT64* tag1 = nullptr, UINT64* tag2 = nullptr);
    virtual HRESULT Flush(UINT64* /*tag1*/ = nullptr, UINT64* /*tag2*/ = nullptr) { return S_OK; }
    virtual void Clear(const D2D1_COLOR_F& clearColor);
    virtual void Clear(const D2D1_COLOR_F* clearColor) { if (clearColor) Clear(*clearColor); }

    virtual void SetTransform(const D2D1_MATRIX_3X2_F& transform) { m_transform = transform; }
    virtual void SetTransform(const D2D1_MATRIX_3X2_F* transform) { if (transform) m_transform = *transform; }
    virtual void GetTransform(D2D1_MATRIX_3X2_F* transform) const { if (transform) *transform = m_transform; }

    virtual void SetAntialiasMode(D2D1_ANTIALIAS_MODE antialiasMode) { m_antialiasMode = antialiasMode; }
    virtual D2D1_ANTIALIAS_MODE GetAntialiasMode() const { return m_antialiasMode; }
    virtual void SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE textAntialiasMode) { m_textAntialiasMode = textAntialiasMode; }
    virtual D2D1_TEXT_ANTIALIAS_MODE GetTextAntialiasMode() const { return m_textAntialiasMode; }
    virtual void SetTextRenderingParams(IDWriteRenderingParams* /*textRenderingParams*/ = nullptr) {}

    virtual void SetDpi(FLOAT dpiX, FLOAT dpiY) {
        m_dpiX = dpiX > 0.f ? dpiX : 96.0f;
        m_dpiY = dpiY > 0.f ? dpiY : 96.0f;
    }
    virtual void GetDpi(FLOAT* dpiX, FLOAT* dpiY) const {
        if (dpiX) *dpiX = m_dpiX;
        if (dpiY) *dpiY = m_dpiY;
    }
    virtual D2D1_SIZE_F GetSize() const {
        if (m_targetBitmap) {
            return D2D1::SizeF((FLOAT)m_targetBitmap->m_width * 96.0f / m_dpiX,
                               (FLOAT)m_targetBitmap->m_height * 96.0f / m_dpiY);
        }
        return D2D1::SizeF(1280.0f, 720.0f);
    }
    virtual D2D1_SIZE_U GetPixelSize() const {
        if (m_targetBitmap) return m_targetBitmap->GetPixelSize();
        return D2D1::SizeU(1280, 720);
    }

    virtual HRESULT CreateSolidColorBrush(const D2D1_COLOR_F& color, ID2D1SolidColorBrush** solidColorBrush) {
        if (!solidColorBrush) return E_POINTER;
        *solidColorBrush = new ID2D1SolidColorBrush(color);
        return S_OK;
    }
    virtual HRESULT CreateSolidColorBrush(const D2D1_COLOR_F& color, const D2D1_BRUSH_PROPERTIES& brushProperties, ID2D1SolidColorBrush** solidColorBrush) {
        if (!solidColorBrush) return E_POINTER;
        auto* b = new ID2D1SolidColorBrush(color);
        b->m_opacity = brushProperties.opacity;
        b->m_transform = brushProperties.transform;
        *solidColorBrush = b;
        return S_OK;
    }
    virtual HRESULT CreateGradientStopCollection(
        const D2D1_GRADIENT_STOP* gradientStops,
        UINT32 gradientStopsCount,
        D2D1_GAMMA colorInterpolationGamma,
        D2D1_EXTEND_MODE extendMode,
        ID2D1GradientStopCollection** gradientStopCollection) {
        if (!gradientStopCollection) return E_POINTER;
        auto* c = new ID2D1GradientStopCollection();
        if (gradientStops && gradientStopsCount > 0) {
            c->m_stops.assign(gradientStops, gradientStops + gradientStopsCount);
        }
        *gradientStopCollection = c;
        return S_OK;
    }
    virtual HRESULT CreateGradientStopCollection(
        const D2D1_GRADIENT_STOP* gradientStops,
        UINT32 gradientStopsCount,
        ID2D1GradientStopCollection** gradientStopCollection) {
        return CreateGradientStopCollection(gradientStops, gradientStopsCount, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, gradientStopCollection);
    }
    virtual HRESULT CreateLinearGradientBrush(
        const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES& linearGradientBrushProperties,
        ID2D1GradientStopCollection* gradientStopCollection,
        ID2D1LinearGradientBrush** linearGradientBrush) {
        if (!linearGradientBrush) return E_POINTER;
        auto* b = new ID2D1LinearGradientBrush();
        b->m_start = linearGradientBrushProperties.startPoint;
        b->m_end = linearGradientBrushProperties.endPoint;
        b->m_stops = gradientStopCollection;
        *linearGradientBrush = b;
        return S_OK;
    }
    virtual HRESULT CreateBitmapBrush(
        ID2D1Bitmap* bitmap,
        const D2D1_BITMAP_BRUSH_PROPERTIES& bitmapBrushProperties,
        ID2D1BitmapBrush** bitmapBrush) {
        if (!bitmapBrush) return E_POINTER;
        auto* b = new ID2D1BitmapBrush();
        b->m_bitmap = bitmap;
        b->m_props = bitmapBrushProperties;
        *bitmapBrush = b;
        return S_OK;
    }
    virtual HRESULT CreateBitmapBrush(
        ID2D1Bitmap* bitmap,
        ID2D1BitmapBrush** bitmapBrush) {
        return CreateBitmapBrush(bitmap, D2D1::BitmapBrushProperties(), bitmapBrush);
    }
    virtual HRESULT CreateBitmap(
        D2D1_SIZE_U size,
        const void* srcData,
        UINT32 pitch,
        const D2D1_BITMAP_PROPERTIES& bitmapProperties,
        ID2D1Bitmap** bitmap) {
        if (!bitmap) return E_POINTER;
        auto* bmp = new ID2D1Bitmap1(size.width, size.height, srcData, pitch);
        bmp->m_dpiX = bitmapProperties.dpiX > 0.f ? bitmapProperties.dpiX : 96.0f;
        bmp->m_dpiY = bitmapProperties.dpiY > 0.f ? bitmapProperties.dpiY : 96.0f;
        *bitmap = bmp;
        return S_OK;
    }
    virtual HRESULT CreateBitmap(
        D2D1_SIZE_U size,
        const void* srcData,
        UINT32 pitch,
        const D2D1_BITMAP_PROPERTIES* bitmapProperties,
        ID2D1Bitmap** bitmap) {
        return CreateBitmap(size, srcData, pitch, bitmapProperties ? *bitmapProperties : D2D1::BitmapProperties(), bitmap);
    }
    virtual HRESULT CreateBitmap(
        D2D1_SIZE_U size,
        const D2D1_BITMAP_PROPERTIES& bitmapProperties,
        ID2D1Bitmap** bitmap) {
        return CreateBitmap(size, nullptr, 0, bitmapProperties, bitmap);
    }
    virtual HRESULT CreateBitmap(
        D2D1_SIZE_U size,
        const D2D1_BITMAP_PROPERTIES* bitmapProperties,
        ID2D1Bitmap** bitmap) {
        return CreateBitmap(size, nullptr, 0, bitmapProperties ? *bitmapProperties : D2D1::BitmapProperties(), bitmap);
    }
    virtual HRESULT CreateBitmapFromWicBitmap(
        IWICBitmapSource* wicBitmapSource,
        const D2D1_BITMAP_PROPERTIES* bitmapProperties,
        ID2D1Bitmap** bitmap);
    virtual HRESULT CreateBitmapFromWicBitmap(
        IWICBitmapSource* wicBitmapSource,
        ID2D1Bitmap** bitmap) {
        return CreateBitmapFromWicBitmap(wicBitmapSource, nullptr, bitmap);
    }
    virtual HRESULT CreateCompatibleRenderTarget(
        D2D1_SIZE_F desiredSize,
        ID2D1BitmapRenderTarget** bitmapRenderTarget);
    virtual HRESULT CreateCompatibleRenderTarget(
        const D2D1_SIZE_F* desiredSize,
        const D2D1_SIZE_U* desiredPixelSize,
        const D2D1_PIXEL_FORMAT* desiredFormat,
        D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS options,
        ID2D1BitmapRenderTarget** bitmapRenderTarget) {
        D2D1_SIZE_F sz = D2D1::SizeF(256.f, 256.f);
        if (desiredSize) sz = *desiredSize;
        else if (desiredPixelSize) sz = D2D1::SizeF((FLOAT)desiredPixelSize->width, (FLOAT)desiredPixelSize->height);
        return CreateCompatibleRenderTarget(sz, bitmapRenderTarget);
    }
    virtual HRESULT CreateLayer(ID2D1Layer** layer) {
        if (!layer) return E_POINTER;
        *layer = new ID2D1Layer();
        return S_OK;
    }

    virtual void PushAxisAlignedClip(const D2D1_RECT_F& clipRect, D2D1_ANTIALIAS_MODE antialiasMode);
    virtual void PushAxisAlignedClip(const D2D1_RECT_F* clipRect, D2D1_ANTIALIAS_MODE antialiasMode) {
        if (clipRect) PushAxisAlignedClip(*clipRect, antialiasMode);
    }
    virtual void PopAxisAlignedClip();

    virtual void PushLayer(const D2D1_LAYER_PARAMETERS& layerParameters, ID2D1Layer* layer);
    virtual void PushLayer(const D2D1_LAYER_PARAMETERS* layerParameters, ID2D1Layer* layer) {
        if (layerParameters) PushLayer(*layerParameters, layer);
    }
    virtual void PushLayer(const D2D1_LAYER_PARAMETERS1& layerParameters, ID2D1Layer* layer);
    virtual void PushLayer(const D2D1_LAYER_PARAMETERS1* layerParameters, ID2D1Layer* layer) {
        if (layerParameters) PushLayer(*layerParameters, layer);
    }
    virtual void PopLayer();

    virtual void FillRectangle(const D2D1_RECT_F& rect, ID2D1Brush* brush);
    virtual void FillRectangle(const D2D1_RECT_F* rect, ID2D1Brush* brush) { if (rect) FillRectangle(*rect, brush); }
    virtual void DrawRectangle(const D2D1_RECT_F& rect, ID2D1Brush* brush, FLOAT strokeWidth = 1.0f, ID2D1StrokeStyle* strokeStyle = nullptr);
    virtual void DrawRectangle(const D2D1_RECT_F* rect, ID2D1Brush* brush, FLOAT strokeWidth = 1.0f, ID2D1StrokeStyle* strokeStyle = nullptr) {
        if (rect) DrawRectangle(*rect, brush, strokeWidth, strokeStyle);
    }

    virtual void FillRoundedRectangle(const D2D1_ROUNDED_RECT& roundedRect, ID2D1Brush* brush);
    virtual void FillRoundedRectangle(const D2D1_ROUNDED_RECT* roundedRect, ID2D1Brush* brush) {
        if (roundedRect) FillRoundedRectangle(*roundedRect, brush);
    }
    virtual void DrawRoundedRectangle(const D2D1_ROUNDED_RECT& roundedRect, ID2D1Brush* brush, FLOAT strokeWidth = 1.0f, ID2D1StrokeStyle* strokeStyle = nullptr);
    virtual void DrawRoundedRectangle(const D2D1_ROUNDED_RECT* roundedRect, ID2D1Brush* brush, FLOAT strokeWidth = 1.0f, ID2D1StrokeStyle* strokeStyle = nullptr) {
        if (roundedRect) DrawRoundedRectangle(*roundedRect, brush, strokeWidth, strokeStyle);
    }

    virtual void FillEllipse(const D2D1_ELLIPSE& ellipse, ID2D1Brush* brush);
    virtual void FillEllipse(const D2D1_ELLIPSE* ellipse, ID2D1Brush* brush) { if (ellipse) FillEllipse(*ellipse, brush); }
    virtual void DrawEllipse(const D2D1_ELLIPSE& ellipse, ID2D1Brush* brush, FLOAT strokeWidth = 1.0f, ID2D1StrokeStyle* strokeStyle = nullptr);
    virtual void DrawEllipse(const D2D1_ELLIPSE* ellipse, ID2D1Brush* brush, FLOAT strokeWidth = 1.0f, ID2D1StrokeStyle* strokeStyle = nullptr) {
        if (ellipse) DrawEllipse(*ellipse, brush, strokeWidth, strokeStyle);
    }

    virtual void DrawLine(D2D1_POINT_2F point0, D2D1_POINT_2F point1, ID2D1Brush* brush, FLOAT strokeWidth = 1.0f, ID2D1StrokeStyle* strokeStyle = nullptr);

    virtual void FillGeometry(ID2D1Geometry* geometry, ID2D1Brush* brush, ID2D1Brush* opacityBrush = nullptr);
    virtual void DrawGeometry(ID2D1Geometry* geometry, ID2D1Brush* brush, FLOAT strokeWidth = 1.0f, ID2D1StrokeStyle* strokeStyle = nullptr);

    virtual void DrawBitmap(
        ID2D1Bitmap* bitmap,
        const D2D1_RECT_F& destinationRectangle,
        FLOAT opacity = 1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE interpolationMode = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
        const D2D1_RECT_F* sourceRectangle = nullptr);
    virtual void DrawBitmap(
        ID2D1Bitmap* bitmap,
        const D2D1_RECT_F* destinationRectangle,
        FLOAT opacity = 1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE interpolationMode = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
        const D2D1_RECT_F* sourceRectangle = nullptr) {
        if (destinationRectangle) DrawBitmap(bitmap, *destinationRectangle, opacity, interpolationMode, sourceRectangle);
        else if (bitmap) {
            D2D1_SIZE_F sz = bitmap->GetSize();
            DrawBitmap(bitmap, D2D1::RectF(0, 0, sz.width, sz.height), opacity, interpolationMode, sourceRectangle);
        }
    }
    virtual void DrawBitmap(
        ID2D1Bitmap* bitmap,
        const D2D1_RECT_F& destinationRectangle,
        FLOAT opacity,
        D2D1_BITMAP_INTERPOLATION_MODE interpolationMode,
        const D2D1_RECT_F& sourceRectangle) {
        DrawBitmap(bitmap, destinationRectangle, opacity, interpolationMode, &sourceRectangle);
    }

    virtual void DrawText(
        const WCHAR* string,
        UINT32 stringLength,
        IDWriteTextFormat* textFormat,
        const D2D1_RECT_F& layoutRect,
        ID2D1Brush* defaultFillBrush,
        D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE,
        DWRITE_MEASURING_MODE measuringMode = DWRITE_MEASURING_MODE_NATURAL);
    virtual void DrawText(
        const WCHAR* string,
        UINT32 stringLength,
        IDWriteTextFormat* textFormat,
        const D2D1_RECT_F* layoutRect,
        ID2D1Brush* defaultFillBrush,
        D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE,
        DWRITE_MEASURING_MODE measuringMode = DWRITE_MEASURING_MODE_NATURAL) {
        if (layoutRect) DrawText(string, stringLength, textFormat, *layoutRect, defaultFillBrush, options, measuringMode);
    }
    virtual void DrawTextW(
        const WCHAR* string,
        UINT32 stringLength,
        IDWriteTextFormat* textFormat,
        const D2D1_RECT_F& layoutRect,
        ID2D1Brush* defaultFillBrush,
        D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE,
        DWRITE_MEASURING_MODE measuringMode = DWRITE_MEASURING_MODE_NATURAL) {
        DrawText(string, stringLength, textFormat, layoutRect, defaultFillBrush, options, measuringMode);
    }
    virtual void DrawTextW(
        const WCHAR* string,
        UINT32 stringLength,
        IDWriteTextFormat* textFormat,
        const D2D1_RECT_F* layoutRect,
        ID2D1Brush* defaultFillBrush,
        D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE,
        DWRITE_MEASURING_MODE measuringMode = DWRITE_MEASURING_MODE_NATURAL) {
        if (layoutRect) DrawText(string, stringLength, textFormat, *layoutRect, defaultFillBrush, options, measuringMode);
    }

    virtual void DrawTextLayout(
        D2D1_POINT_2F origin,
        IDWriteTextLayout* textLayout,
        ID2D1Brush* defaultFillBrush,
        D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE);
};

struct ID2D1BitmapRenderTarget : public ID2D1RenderTarget {
    ID2D1BitmapRenderTarget(UINT32 w = 256, UINT32 h = 256) {
        m_targetBitmap.Attach(new ID2D1Bitmap1(w, h));
    }
    virtual HRESULT GetBitmap(ID2D1Bitmap** bitmap) {
        if (!bitmap) return E_POINTER;
        *bitmap = m_targetBitmap.Get();
        if (*bitmap) (*bitmap)->AddRef();
        return S_OK;
    }
};

struct ID2D1DCRenderTarget : public ID2D1RenderTarget {
    virtual HRESULT BindDC(const HDC hDC, const RECT* pSubRect);
};

struct ID2D1Device;

struct ID2D1DeviceContext : public ID2D1RenderTarget {
    ID2D1Device* m_device = nullptr;
    virtual void GetDevice(ID2D1Device** device) const;
    virtual void SetTarget(ID2D1Image* image) {
        if (!image) {
            m_targetBitmap.Reset();
            return;
        }
        m_targetBitmap = static_cast<ID2D1Bitmap1*>(image);
    }
    virtual void GetTarget(ID2D1Image** image) const {
        if (!image) return;
        *image = m_targetBitmap.Get();
        if (*image) (*image)->AddRef();
    }
    virtual HRESULT CreateBitmapFromDxgiSurface(
        IDXGISurface* surface,
        const D2D1_BITMAP_PROPERTIES1* bitmapProperties,
        ID2D1Bitmap1** bitmap) {
        if (!bitmap || !surface) return E_POINTER;
        auto* bmp = new ID2D1Bitmap1(surface->m_width, surface->m_height);
        if (bitmapProperties) {
            bmp->m_dpiX = bitmapProperties->dpiX > 0.f ? bitmapProperties->dpiX : 96.0f;
            bmp->m_dpiY = bitmapProperties->dpiY > 0.f ? bitmapProperties->dpiY : 96.0f;
        }
        *bitmap = bmp;
        return S_OK;
    }
    virtual HRESULT CreateBitmapFromDxgiSurface(
        IDXGISurface* surface,
        const D2D1_BITMAP_PROPERTIES1& bitmapProperties,
        ID2D1Bitmap1** bitmap) {
        return CreateBitmapFromDxgiSurface(surface, &bitmapProperties, bitmap);
    }
    virtual HRESULT CreateBitmap(
        D2D1_SIZE_U size,
        const void* sourceData,
        UINT32 pitch,
        const D2D1_BITMAP_PROPERTIES1& bitmapProperties,
        ID2D1Bitmap1** bitmap) {
        if (!bitmap) return E_POINTER;
        auto* bmp = new ID2D1Bitmap1(size.width, size.height, sourceData, pitch);
        bmp->m_dpiX = bitmapProperties.dpiX > 0.f ? bitmapProperties.dpiX : 96.0f;
        bmp->m_dpiY = bitmapProperties.dpiY > 0.f ? bitmapProperties.dpiY : 96.0f;
        *bitmap = bmp;
        return S_OK;
    }
    virtual HRESULT CreateBitmap(
        D2D1_SIZE_U size,
        const void* sourceData,
        UINT32 pitch,
        const D2D1_BITMAP_PROPERTIES1* bitmapProperties,
        ID2D1Bitmap1** bitmap) {
        return CreateBitmap(size, sourceData, pitch, bitmapProperties ? *bitmapProperties : D2D1::BitmapProperties1(), bitmap);
    }
    using ID2D1RenderTarget::CreateBitmap;

    virtual HRESULT CreateEffect(REFCLSID effectId, ID2D1Effect** effect) {
        if (!effect) return E_POINTER;
        *effect = new ID2D1Effect();
        return S_OK;
    }

    using ID2D1RenderTarget::DrawBitmap;
    virtual void DrawBitmap(
        ID2D1Bitmap* bitmap,
        const D2D1_RECT_F& destinationRectangle,
        FLOAT opacity,
        D2D1_INTERPOLATION_MODE interpolationMode,
        const D2D1_RECT_F* sourceRectangle = nullptr,
        const void* perspectiveTransform = nullptr) {
        ID2D1RenderTarget::DrawBitmap(bitmap, destinationRectangle, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, sourceRectangle);
    }
    virtual void DrawBitmap(
        ID2D1Bitmap* bitmap,
        const D2D1_RECT_F* destinationRectangle,
        FLOAT opacity,
        D2D1_INTERPOLATION_MODE interpolationMode,
        const D2D1_RECT_F* sourceRectangle = nullptr,
        const void* perspectiveTransform = nullptr) {
        if (destinationRectangle) {
            ID2D1RenderTarget::DrawBitmap(bitmap, *destinationRectangle, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, sourceRectangle);
        }
    }
    virtual void DrawImage(
        ID2D1Image* image,
        const D2D1_POINT_2F* targetOffset = nullptr,
        const D2D1_RECT_F* imageRectangle = nullptr,
        D2D1_INTERPOLATION_MODE interpolationMode = D2D1_INTERPOLATION_MODE_LINEAR,
        D2D1_COMPOSITE_MODE compositeMode = D2D1_COMPOSITE_MODE_SOURCE_OVER) {}
    virtual void DrawImage(
        ID2D1Effect* effect,
        const D2D1_POINT_2F* targetOffset = nullptr,
        const D2D1_RECT_F* imageRectangle = nullptr,
        D2D1_INTERPOLATION_MODE interpolationMode = D2D1_INTERPOLATION_MODE_LINEAR,
        D2D1_COMPOSITE_MODE compositeMode = D2D1_COMPOSITE_MODE_SOURCE_OVER) {}
};

struct ID2D1Device : public ID2D1Resource {
    virtual HRESULT CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS options, ID2D1DeviceContext** deviceContext) {
        if (!deviceContext) return E_POINTER;
        auto* ctx = new ID2D1DeviceContext();
        ctx->m_device = this;
        *deviceContext = ctx;
        return S_OK;
    }
};

inline void ID2D1DeviceContext::GetDevice(ID2D1Device** device) const {
    if (!device) return;
    *device = m_device;
    if (*device) (*device)->AddRef();
}

struct ID2D1Factory : public IUnknown {
    virtual HRESULT CreatePathGeometry(ID2D1PathGeometry** pathGeometry) {
        if (!pathGeometry) return E_POINTER;
        *pathGeometry = new ID2D1PathGeometry();
        return S_OK;
    }
    virtual HRESULT CreateRoundedRectangleGeometry(
        const D2D1_ROUNDED_RECT& roundedRectangle,
        ID2D1RoundedRectangleGeometry** roundedRectangleGeometry) {
        if (!roundedRectangleGeometry) return E_POINTER;
        *roundedRectangleGeometry = new ID2D1RoundedRectangleGeometry(roundedRectangle);
        return S_OK;
    }
    virtual HRESULT CreateRoundedRectangleGeometry(
        const D2D1_ROUNDED_RECT* roundedRectangle,
        ID2D1RoundedRectangleGeometry** roundedRectangleGeometry) {
        return CreateRoundedRectangleGeometry(roundedRectangle ? *roundedRectangle : D2D1_ROUNDED_RECT{}, roundedRectangleGeometry);
    }
    virtual HRESULT CreateDCRenderTarget(
        const D2D1_RENDER_TARGET_PROPERTIES* renderTargetProperties,
        ID2D1DCRenderTarget** dcRenderTarget) {
        if (!dcRenderTarget) return E_POINTER;
        *dcRenderTarget = new ID2D1DCRenderTarget();
        return S_OK;
    }
    virtual HRESULT CreateStrokeStyle(
        const D2D1_STROKE_STYLE_PROPERTIES& strokeStyleProperties,
        const FLOAT* dashes,
        UINT32 dashesCount,
        ID2D1StrokeStyle** strokeStyle) {
        if (!strokeStyle) return E_POINTER;
        *strokeStyle = new ID2D1StrokeStyle();
        return S_OK;
    }
    virtual HRESULT CreateStrokeStyle(
        const D2D1_STROKE_STYLE_PROPERTIES* strokeStyleProperties,
        const FLOAT* dashes,
        UINT32 dashesCount,
        ID2D1StrokeStyle** strokeStyle) {
        if (!strokeStyle) return E_POINTER;
        *strokeStyle = new ID2D1StrokeStyle();
        return S_OK;
    }
};

struct ID2D1Factory1 : public ID2D1Factory {
    virtual HRESULT CreateDevice(IDXGIDevice* dxgiDevice, ID2D1Device** d2dDevice) {
        if (!d2dDevice) return E_POINTER;
        *d2dDevice = new ID2D1Device();
        return S_OK;
    }
};

inline void ID2D1Resource::GetFactory(ID2D1Factory** factory) const {
    if (!factory) return;
    *factory = new ID2D1Factory1();
}

struct D2D1_FACTORY_OPTIONS {
    int debugLevel = 0;
};

inline HRESULT D2D1CreateFactory(
    D2D1_FACTORY_TYPE factoryType,
    REFIID riid,
    const void* pFactoryOptions,
    void** ppIFactory) {
    if (!ppIFactory) return E_POINTER;
    *ppIFactory = new ID2D1Factory1();
    return S_OK;
}

template <typename FactoryT>
inline HRESULT D2D1CreateFactory(
    D2D1_FACTORY_TYPE factoryType,
    REFIID riid,
    const void* pFactoryOptions,
    FactoryT** ppIFactory) {
    if (!ppIFactory) return E_POINTER;
    *ppIFactory = new FactoryT();
    return S_OK;
}

template <typename FactoryT>
inline HRESULT D2D1CreateFactory(
    D2D1_FACTORY_TYPE factoryType,
    FactoryT** ppIFactory) {
    if (!ppIFactory) return E_POINTER;
    *ppIFactory = new FactoryT();
    return S_OK;
}

template <typename FactoryT>
inline HRESULT DWriteCreateFactory(DWRITE_FACTORY_TYPE factoryType, REFIID iid, FactoryT** factory) {
    if (!factory) return E_POINTER;
    *factory = static_cast<FactoryT*>(new IDWriteFactory5());
    return S_OK;
}

#endif // !defined(_WIN32)
