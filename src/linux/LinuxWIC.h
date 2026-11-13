#pragma once

#if !defined(_WIN32)

#include "LinuxD2D.h"

// ============================================================================
// Windows Imaging Component (WIC)
// ============================================================================

using WICPixelFormatGUID = GUID;
using REFWICPixelFormatGUID = const GUID&;
using LPOLESTR = WCHAR*;
using BSTR = WCHAR*;

struct SAFEARRAY;

enum VARENUM {
    VT_EMPTY = 0,
    VT_NULL = 1,
    VT_I2 = 2,
    VT_I4 = 3,
    VT_R4 = 4,
    VT_R8 = 5,
    VT_BSTR = 8,
    VT_DISPATCH = 9,
    VT_BOOL = 11,
    VT_VARIANT = 12,
    VT_UNKNOWN = 13,
    VT_UI1 = 17,
    VT_UI2 = 18,
    VT_UI4 = 19,
    VT_I8 = 20,
    VT_UI8 = 21,
    VT_LPSTR = 30,
    VT_LPWSTR = 31,
    VT_ARRAY = 0x2000,
};

struct VARIANT {
    VARTYPE vt = VT_EMPTY;
    WORD wReserved1 = 0;
    WORD wReserved2 = 0;
    WORD wReserved3 = 0;
    union {
        LONG lVal;
        ULONG ulVal;
        SHORT boolVal;
        BYTE bVal;
        FLOAT fltVal;
        double dblVal;
        BSTR bstrVal;
        IUnknown* punkVal;
        SAFEARRAY* parray;
    };
};

struct PROPBAG2 {
    DWORD dwType = 0;
    VARTYPE vt = VT_EMPTY;
     WORD cfType = 0;
    DWORD dwHint = 0;
    LPOLESTR pstrName = nullptr;
    CLSID clsid{};
};

inline constexpr GUID CLSID_WICImagingFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
inline constexpr GUID GUID_WICPixelFormat32bppPBGRA = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};
inline constexpr GUID GUID_WICPixelFormat32bppBGRA = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f}};
inline constexpr GUID GUID_WICPixelFormat32bppRGBA = {0xf5c7ad2d, 0x6a8d, 0x43dd, {0xa7, 0xa8, 0xa2, 0x99, 0x35, 0x26, 0x1a, 0xe9}};
inline constexpr GUID GUID_WICPixelFormat24bppBGR = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};
inline constexpr GUID GUID_ContainerFormatPng = {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
inline constexpr GUID GUID_ContainerFormatJpeg = {0x19e4a5aa, 0x5662, 0x4fc5, {0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57}};
#define WICBitmapEncoderNoCache 0x2
#define WICJpegYCrCbSubsampling444 3
#define STATFLAG_NONAME 1
#define STREAM_SEEK_SET 0
#define STREAM_SEEK_CUR 1
#define STREAM_SEEK_END 2

struct STATSTG {
    LPOLESTR pwcsName = nullptr;
    DWORD type = 0;
    ULARGE_INTEGER cbSize{};
};

struct IStream : public IUnknown {
    std::vector<uint8_t> m_data;
    size_t m_pos = 0;

    virtual HRESULT Read(void* pv, ULONG cb, ULONG* pcbRead) {
        if (!pv) return E_POINTER;
        size_t avail = (m_pos < m_data.size()) ? (m_data.size() - m_pos) : 0;
        size_t toRead = std::min<size_t>(cb, avail);
        if (toRead > 0) {
            std::memcpy(pv, m_data.data() + m_pos, toRead);
            m_pos += toRead;
        }
        if (pcbRead) *pcbRead = static_cast<ULONG>(toRead);
        return S_OK;
    }
    virtual HRESULT Write(const void* pv, ULONG cb, ULONG* pcbWritten) {
        if (!pv && cb > 0) return E_POINTER;
        if (m_pos + cb > m_data.size()) m_data.resize(m_pos + cb);
        if (cb > 0) {
            std::memcpy(m_data.data() + m_pos, pv, cb);
            m_pos += cb;
        }
        if (pcbWritten) *pcbWritten = cb;
        return S_OK;
    }
    virtual HRESULT Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition) {
        int64_t base = 0;
        if (dwOrigin == STREAM_SEEK_CUR) base = static_cast<int64_t>(m_pos);
        else if (dwOrigin == STREAM_SEEK_END) base = static_cast<int64_t>(m_data.size());
        int64_t next = base + dlibMove.QuadPart;
        if (next < 0) return E_INVALIDARG;
        m_pos = static_cast<size_t>(next);
        if (plibNewPosition) plibNewPosition->QuadPart = static_cast<ULONGLONG>(m_pos);
        return S_OK;
    }
    virtual HRESULT Stat(STATSTG* pstatstg, DWORD /*grfStatFlag*/) {
        if (!pstatstg) return E_POINTER;
        pstatstg->cbSize.QuadPart = static_cast<ULONGLONG>(m_data.size());
        return S_OK;
    }
};

inline HRESULT CreateStreamOnHGlobal(HGLOBAL /*hGlobal*/, BOOL /*fDeleteOnRelease*/, IStream** ppstm) {
    if (!ppstm) return E_POINTER;
    *ppstm = new IStream();
    return S_OK;
}

struct IPropertyBag2 : public IUnknown {
    float m_jpegQuality = 0.80f;
    virtual HRESULT Write(ULONG cProperties, PROPBAG2* pPropBag, VARIANT* pvarValue) {
        if (pPropBag && pvarValue) {
            for (ULONG i = 0; i < cProperties; ++i) {
                if (pPropBag[i].pstrName && std::wcscmp(pPropBag[i].pstrName, L"ImageQuality") == 0 && pvarValue[i].vt == VT_R4) {
                    m_jpegQuality = pvarValue[i].fltVal;
                }
            }
        }
        return S_OK;
    }
};

struct IWICBitmapFrameEncode : public IUnknown {
    Microsoft::WRL::ComPtr<IStream> m_stream;
    GUID m_containerFormat = GUID_ContainerFormatPng;
    float m_jpegQuality = 0.80f;
    UINT m_width = 0;
    UINT m_height = 0;
    GUID m_format = GUID_WICPixelFormat32bppBGRA;

    virtual HRESULT Initialize(IPropertyBag2* pIEncoderOptions) {
        if (pIEncoderOptions) m_jpegQuality = pIEncoderOptions->m_jpegQuality;
        return S_OK;
    }
    virtual HRESULT SetSize(UINT uiWidth, UINT uiHeight) {
        m_width = uiWidth;
        m_height = uiHeight;
        return S_OK;
    }
    virtual HRESULT SetPixelFormat(WICPixelFormatGUID* pPixelFormat) {
        if (!pPixelFormat) return E_POINTER;
        m_format = *pPixelFormat;
        return S_OK;
    }
    virtual HRESULT WritePixels(UINT lineCount, UINT cbStride, UINT cbBufferSize, BYTE* pbPixels);
    virtual HRESULT Commit() { return S_OK; }
};

struct IWICBitmapEncoder : public IUnknown {
    Microsoft::WRL::ComPtr<IStream> m_stream;
    GUID m_containerFormat = GUID_ContainerFormatPng;
    virtual HRESULT Initialize(IUnknown* pIStream, DWORD /*cacheOption*/) {
        m_stream = static_cast<IStream*>(pIStream);
        return m_stream ? S_OK : E_INVALIDARG;
    }
    virtual HRESULT CreateNewFrame(IWICBitmapFrameEncode** ppIFramEncode, IPropertyBag2** ppIEncoderOptions) {
        if (ppIFramEncode) {
            auto* f = new IWICBitmapFrameEncode();
            f->m_stream = m_stream;
            f->m_containerFormat = m_containerFormat;
            *ppIFramEncode = f;
        }
        if (ppIEncoderOptions) *ppIEncoderOptions = new IPropertyBag2();
        return S_OK;
    }
    virtual HRESULT Commit() { return S_OK; }
};

enum WICDecodeOptions {
    WICDecodeMetadataCacheOnDemand = 0,
    WICDecodeMetadataCacheOnLoad = 0x1,
};

enum WICBitmapDitherType {
    WICBitmapDitherTypeNone = 0,
};

enum WICBitmapPaletteType {
    WICBitmapPaletteTypeCustom = 0,
    WICBitmapPaletteTypeMedianCut = 0x1,
};

enum WICBitmapInterpolationMode {
    WICBitmapInterpolationModeNearestNeighbor = 0,
    WICBitmapInterpolationModeLinear = 0x1,
    WICBitmapInterpolationModeCubic = 0x2,
    WICBitmapInterpolationModeFant = 0x3,
    WICBitmapInterpolationModeHighQualityCubic = 0x4,
};

struct IWICPalette : public IUnknown {};

struct IWICBitmapSource : public IUnknown {
    UINT m_width = 1;
    UINT m_height = 1;
    bool m_isPremultiplied = false;
    std::vector<uint8_t> m_pixels; // BGRA 32bpp

    virtual HRESULT GetSize(UINT* puiWidth, UINT* puiHeight) {
        if (puiWidth) *puiWidth = m_width;
        if (puiHeight) *puiHeight = m_height;
        return S_OK;
    }
    virtual HRESULT CopyPixels(const void* prc, UINT cbStride, UINT cbBufferSize, BYTE* pbBuffer) {
        if (!pbBuffer) return E_POINTER;
        size_t rowBytes = (size_t)m_width * 4;
        if (m_pixels.size() < rowBytes * m_height) return E_FAIL;
        for (UINT y = 0; y < m_height; ++y) {
            if ((size_t)y * cbStride + rowBytes <= cbBufferSize) {
                std::memcpy(pbBuffer + (size_t)y * cbStride, m_pixels.data() + (size_t)y * rowBytes, rowBytes);
            }
        }
        return S_OK;
    }
};

struct IWICBitmapFrameDecode : public IWICBitmapSource {};

struct IWICFormatConverter : public IWICBitmapSource {
    virtual HRESULT Initialize(
        IWICBitmapSource* pISource,
        REFWICPixelFormatGUID dstFormat,
        WICBitmapDitherType /*dither*/,
        IWICPalette* /*pIPalette*/,
        double /*alphaThresholdPercent*/,
        WICBitmapPaletteType /*paletteTranslate*/) {
        if (!pISource) return E_INVALIDARG;
        m_width = pISource->m_width;
        m_height = pISource->m_height;
        m_pixels = pISource->m_pixels;
        const bool wantPremul = IsEqualGUID(dstFormat, GUID_WICPixelFormat32bppPBGRA);
        const bool wantStraight = IsEqualGUID(dstFormat, GUID_WICPixelFormat32bppBGRA);
        if (wantPremul && !pISource->m_isPremultiplied) {
            for (size_t i = 0; i + 3 < m_pixels.size(); i += 4) {
                uint32_t a = m_pixels[i + 3];
                if (a == 0) {
                    m_pixels[i + 0] = m_pixels[i + 1] = m_pixels[i + 2] = 0;
                } else if (a < 255) {
                    m_pixels[i + 0] = static_cast<uint8_t>((m_pixels[i + 0] * a + 127) / 255);
                    m_pixels[i + 1] = static_cast<uint8_t>((m_pixels[i + 1] * a + 127) / 255);
                    m_pixels[i + 2] = static_cast<uint8_t>((m_pixels[i + 2] * a + 127) / 255);
                }
            }
            m_isPremultiplied = true;
        } else if (wantStraight && pISource->m_isPremultiplied) {
            for (size_t i = 0; i + 3 < m_pixels.size(); i += 4) {
                uint32_t a = m_pixels[i + 3];
                if (a > 0 && a < 255) {
                    m_pixels[i + 0] = static_cast<uint8_t>(std::min<uint32_t>(255, (m_pixels[i + 0] * 255 + a / 2) / a));
                    m_pixels[i + 1] = static_cast<uint8_t>(std::min<uint32_t>(255, (m_pixels[i + 1] * 255 + a / 2) / a));
                    m_pixels[i + 2] = static_cast<uint8_t>(std::min<uint32_t>(255, (m_pixels[i + 2] * 255 + a / 2) / a));
                }
            }
            m_isPremultiplied = false;
        } else {
            m_isPremultiplied = pISource->m_isPremultiplied;
        }
        return S_OK;
    }
};

struct IWICBitmapScaler : public IWICBitmapSource {
    virtual HRESULT Initialize(
        IWICBitmapSource* pISource,
        UINT uiWidth,
        UINT uiHeight,
        WICBitmapInterpolationMode mode);
};

struct IWICStream : public IStream {
    virtual HRESULT InitializeFromMemory(BYTE* pbBuffer, DWORD cbBufferSize) {
        if (!pbBuffer || cbBufferSize == 0) return E_INVALIDARG;
        m_data.assign(pbBuffer, pbBuffer + cbBufferSize);
        m_pos = 0;
        return S_OK;
    }
    virtual HRESULT InitializeFromFilename(LPCWSTR wzFileName, DWORD dwDesiredAccess);
};

struct IWICBitmapDecoder : public IUnknown {
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> m_frame;
    virtual HRESULT Initialize(IUnknown* pIStream, WICDecodeOptions cacheOptions);
    virtual HRESULT GetFrame(UINT /*index*/, IWICBitmapFrameDecode** ppIBitmapFrame) {
        if (!ppIBitmapFrame) return E_POINTER;
        if (!m_frame) return E_FAIL;
        *ppIBitmapFrame = m_frame.Get();
        m_frame->AddRef();
        return S_OK;
    }
};

struct IWICImagingFactory : public IUnknown {
    virtual HRESULT CreateStream(IWICStream** ppIWICStream) {
        if (!ppIWICStream) return E_POINTER;
        *ppIWICStream = new IWICStream();
        return S_OK;
    }
    virtual HRESULT CreateDecoder(REFGUID /*guidContainerFormat*/, const GUID* /*pguidVendor*/, IWICBitmapDecoder** ppIDecoder) {
        if (!ppIDecoder) return E_POINTER;
        *ppIDecoder = new IWICBitmapDecoder();
        return S_OK;
    }
    virtual HRESULT CreateDecoderFromStream(
        IUnknown* pIStream,
        const GUID* pguidVendor,
        WICDecodeOptions metadataOptions,
        IWICBitmapDecoder** ppIDecoder);
    virtual HRESULT CreateDecoderFromFilename(
        LPCWSTR wzFilename,
        const GUID* pguidVendor,
        DWORD dwDesiredAccess,
        WICDecodeOptions metadataOptions,
        IWICBitmapDecoder** ppIDecoder);
    virtual HRESULT CreateFormatConverter(IWICFormatConverter** ppIFormatConverter) {
        if (!ppIFormatConverter) return E_POINTER;
        *ppIFormatConverter = new IWICFormatConverter();
        return S_OK;
    }
    virtual HRESULT CreateBitmapScaler(IWICBitmapScaler** ppIBitmapScaler) {
        if (!ppIBitmapScaler) return E_POINTER;
        *ppIBitmapScaler = new IWICBitmapScaler();
        return S_OK;
    }
    virtual HRESULT CreateEncoder(REFGUID guidContainerFormat, const GUID* /*pguidVendor*/, IWICBitmapEncoder** ppIEncoder) {
        if (!ppIEncoder) return E_POINTER;
        auto* enc = new IWICBitmapEncoder();
        enc->m_containerFormat = guidContainerFormat;
        *ppIEncoder = enc;
        return S_OK;
    }
};

// ============================================================================
// Shell / File Dialog / PropertyStore / JumpList COM Interfaces
// ============================================================================

inline constexpr GUID CLSID_FileOpenDialog = {0xdc1c5a9c, 0xe88a, 0x4dde, {0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7}};
inline constexpr GUID CLSID_FileSaveDialog = {0xc0b4e2f3, 0xba21, 0x4773, {0x8d, 0xba, 0x33, 0x5e, 0xc9, 0x46, 0xeb, 0x8b}};
inline constexpr GUID CLSID_DestinationList = {0x77f10cf0, 0x3db5, 0x4966, {0xb5, 0x20, 0xb7, 0xc5, 0x4f, 0xd3, 0x5e, 0xd6}};
inline constexpr GUID CLSID_EnumerableObjectCollection = {0x2d3468c1, 0x36a7, 0x43b6, {0xac, 0x24, 0xd3, 0xf0, 0x2f, 0xd9, 0x60, 0x7a}};
inline constexpr GUID CLSID_ShellLink = {0x00021401, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
inline constexpr GUID CLSID_TaskbarList = {0x56fdf344, 0xfd6d, 0x11d0, {0x95, 0x8a, 0x00, 0x60, 0x97, 0xc9, 0xa0, 0x90}};
inline constexpr GUID CLSID_ApplicationAssociationRegistrationUI = {0x1968106d, 0xf3b5, 0x44cf, {0x89, 0x0e, 0x11, 0x6f, 0xcb, 0x9e, 0xce, 0xf1}};

enum SIGDN {
    SIGDN_NORMALDISPLAY = 0,
    SIGDN_FILESYSPATH = (int)0x80058000,
};

enum _FILEOPENDIALOGOPTIONS {
    FOS_OVERWRITEPROMPT = 0x2,
    FOS_STRICTFILESPECS = 0x4,
    FOS_NOCHANGEDIR = 0x8,
    FOS_PICKFOLDERS = 0x20,
    FOS_FORCEFILESYSTEM = 0x40,
    FOS_ALLNONSTORAGEITEMS = 0x80,
    FOS_NOVALIDATE = 0x100,
    FOS_ALLOWMULTISELECT = 0x200,
    FOS_PATHMUSTEXIST = 0x800,
    FOS_FILEMUSTEXIST = 0x1000,
    FOS_CREATEPROMPT = 0x2000,
    FOS_SHAREAWARE = 0x4000,
    FOS_NOREADONLYRETURN = 0x8000,
    FOS_NOTESTFILECREATE = 0x10000,
    FOS_HIDEMRUPLACES = 0x20000,
    FOS_HIDEPINNEDPLACES = 0x40000,
    FOS_NODEREFERENCELINKS = 0x100000,
    FOS_OKBUTTONNEEDSINTERACTION = 0x200000,
    FOS_DONTADDTORECENT = 0x2000000,
    FOS_FORCESHOWHIDDEN = 0x10000000,
    FOS_DEFAULTNOMINIMODE = 0x20000000,
    FOS_FORCEPREVIEWPANEON = 0x40000000,
};
using FILEOPENDIALOGOPTIONS = DWORD;

struct COMDLG_FILTERSPEC {
    LPCWSTR pszName;
    LPCWSTR pszSpec;
};

struct IShellItem : public IUnknown {
    std::wstring m_path;
    IShellItem(const std::wstring& p = L"") : m_path(p) {}
    virtual HRESULT GetDisplayName(SIGDN sigdnName, LPWSTR* ppszName);
};

struct IShellItemArray : public IUnknown {
    std::vector<Microsoft::WRL::ComPtr<IShellItem>> m_items;
    virtual HRESULT GetCount(DWORD* pdwNumItems) {
        if (!pdwNumItems) return E_POINTER;
        *pdwNumItems = (DWORD)m_items.size();
        return S_OK;
    }
    virtual HRESULT GetItemAt(DWORD dwIndex, IShellItem** ppsi) {
        if (!ppsi || dwIndex >= m_items.size()) return E_INVALIDARG;
        *ppsi = m_items[dwIndex].Get();
        (*ppsi)->AddRef();
        return S_OK;
    }
};

struct IFileDialog : public IUnknown {
    DWORD m_options = 0;
    std::wstring m_title;
    std::wstring m_initialFolder;
    std::wstring m_defaultExt;
    std::wstring m_resultPath;
    std::vector<std::pair<std::wstring, std::wstring>> m_filters;

    virtual HRESULT Show(HWND hwndOwner);
    virtual HRESULT SetFileTypes(UINT cFileTypes, const COMDLG_FILTERSPEC* rgFilterSpec) {
        m_filters.clear();
        if (rgFilterSpec) {
            for (UINT i = 0; i < cFileTypes; ++i) {
                std::wstring n = rgFilterSpec[i].pszName ? rgFilterSpec[i].pszName : L"";
                std::wstring s = rgFilterSpec[i].pszSpec ? rgFilterSpec[i].pszSpec : L"";
                if (!s.empty()) m_filters.emplace_back(std::move(n), std::move(s));
            }
        }
        return S_OK;
    }
    virtual HRESULT SetFileTypeIndex(UINT /*iFileType*/) { return S_OK; }
    virtual HRESULT SetOptions(FILEOPENDIALOGOPTIONS fos) { m_options = fos; return S_OK; }
    virtual HRESULT GetOptions(FILEOPENDIALOGOPTIONS* pfos) { if (pfos) *pfos = m_options; return S_OK; }
    virtual HRESULT SetTitle(LPCWSTR pszTitle) { if (pszTitle) m_title = pszTitle; return S_OK; }
    virtual HRESULT SetFolder(IShellItem* psi) {
        if (psi) m_initialFolder = psi->m_path;
        return S_OK;
    }
    virtual HRESULT SetDefaultExtension(LPCWSTR pszDefaultExtension) {
        if (pszDefaultExtension) m_defaultExt = pszDefaultExtension;
        return S_OK;
    }
    virtual HRESULT GetResult(IShellItem** ppsi) {
        if (!ppsi) return E_POINTER;
        if (m_resultPath.empty()) return E_FAIL;
        *ppsi = new IShellItem(m_resultPath);
        return S_OK;
    }
};

struct IFileOpenDialog : public IFileDialog {
    std::vector<std::wstring> m_resultPaths;
    virtual HRESULT GetResults(IShellItemArray** ppenum) {
        if (!ppenum) return E_POINTER;
        auto* arr = new IShellItemArray();
        for (const auto& p : m_resultPaths) {
            Microsoft::WRL::ComPtr<IShellItem> item;
            item.Attach(new IShellItem(p));
            arr->m_items.push_back(item);
        }
        *ppenum = arr;
        return S_OK;
    }
};

struct PROPERTYKEY {
    GUID fmtid;
    DWORD pid;
};
using REFPROPERTYKEY = const PROPERTYKEY&;

struct PROPVARIANT {
    VARTYPE vt = VT_EMPTY;
    WORD wReserved1 = 0;
    WORD wReserved2 = 0;
    WORD wReserved3 = 0;
    union {
        LONG lVal;
        ULONG ulVal;
        SHORT boolVal;
        LARGE_INTEGER hVal;
        ULARGE_INTEGER uhVal;
        LPWSTR pwszVal;
        LPSTR pszVal;
    };
};

// MediaFoundation types for MediaLibrary.cpp fallback
#define MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING GUID_NULL
#define MF_PD_DURATION GUID_NULL
#define MF_MT_FRAME_SIZE GUID_NULL
#define MF_SOURCE_READER_MEDIASOURCE 0xFFFFFFFFUL
#define MF_SOURCE_READER_FIRST_VIDEO_STREAM 0xFFFFFFFCUL

struct IMFAttributes : public IUnknown {
    virtual HRESULT SetUINT32(REFGUID /*guidKey*/, UINT32 /*unValue*/) { return S_OK; }
    virtual HRESULT GetUINT64(REFGUID /*guidKey*/, UINT64* punValue) {
        if (punValue) *punValue = 0;
        return E_FAIL;
    }
};

struct IMFMediaType : public IMFAttributes {};

struct IMFSourceReader : public IUnknown {
    virtual HRESULT GetPresentationAttribute(DWORD /*dwStreamIndex*/, REFGUID /*guidAttribute*/, PROPVARIANT* /*pvarAttribute*/) { return E_FAIL; }
    virtual HRESULT GetNativeMediaType(DWORD /*dwStreamIndex*/, DWORD /*dwMediaTypeIndex*/, IMFMediaType** /*ppMediaType*/) { return E_FAIL; }
};

inline HRESULT MFCreateAttributes(IMFAttributes** ppMFAttributes, UINT32 /*cInitialSize*/) {
    if (!ppMFAttributes) return E_POINTER;
    *ppMFAttributes = nullptr;
    return E_FAIL;
}
inline HRESULT MFCreateSourceReaderFromURL(LPCWSTR /*pwszURL*/, IMFAttributes* /*pAttributes*/, IMFSourceReader** ppSourceReader) {
    if (!ppSourceReader) return E_POINTER;
    *ppSourceReader = nullptr;
    return E_FAIL;
}
inline HRESULT MFGetAttributeSize(IMFAttributes* /*pAttributes*/, REFGUID /*guidKey*/, UINT32* punWidth, UINT32* punHeight) {
    if (punWidth) *punWidth = 0;
    if (punHeight) *punHeight = 0;
    return E_FAIL;
}
using REFPROPVARIANT = const PROPVARIANT&;

inline void PropVariantInit(PROPVARIANT* pvar) {
    if (pvar) std::memset(pvar, 0, sizeof(PROPVARIANT));
}
inline HRESULT PropVariantClear(PROPVARIANT* pvar) {
    if (pvar) std::memset(pvar, 0, sizeof(PROPVARIANT));
    return S_OK;
}

struct IPropertyStore : public IUnknown {
    virtual HRESULT SetValue(REFPROPERTYKEY key, REFPROPVARIANT propvar) { return S_OK; }
    virtual HRESULT GetValue(REFPROPERTYKEY key, PROPVARIANT* pv) {
        if (pv) PropVariantInit(pv);
        return S_OK;
    }
    virtual HRESULT Commit() { return S_OK; }
};

struct IShellLinkW : public IUnknown {
    virtual HRESULT SetPath(LPCWSTR pszFile) { return S_OK; }
    virtual HRESULT SetArguments(LPCWSTR pszArgs) { return S_OK; }
    virtual HRESULT SetIconLocation(LPCWSTR pszIconPath, int iIcon) { return S_OK; }
    virtual HRESULT SetDescription(LPCWSTR pszName) { return S_OK; }
    virtual HRESULT SetWorkingDirectory(LPCWSTR pszDir) { return S_OK; }
};

struct IPersistFile : public IUnknown {
    virtual HRESULT Save(LPCWSTR /*pszFileName*/, BOOL /*fRemember*/) { return S_OK; }
    virtual HRESULT Load(LPCWSTR /*pszFileName*/, DWORD /*dwMode*/) { return S_OK; }
};

struct IObjectArray : public IUnknown {
    virtual HRESULT GetCount(UINT* pcObjects) { if (pcObjects) *pcObjects = 0; return S_OK; }
    virtual HRESULT GetAt(UINT uiIndex, REFIID riid, void** ppv) { return E_FAIL; }
};

struct IObjectCollection : public IObjectArray {
    virtual HRESULT AddObject(IUnknown* punk) { return S_OK; }
    virtual HRESULT Clear() { return S_OK; }
};

struct ICustomDestinationList : public IUnknown {
    virtual HRESULT SetAppID(LPCWSTR pszAppID) { return S_OK; }
    virtual HRESULT BeginList(UINT* pcMinSlots, REFIID riid, void** ppv) {
        if (pcMinSlots) *pcMinSlots = 10;
        if (ppv) *ppv = new IObjectArray();
        return S_OK;
    }
    virtual HRESULT AppendCategory(LPCWSTR pszCategory, IObjectArray* poa) { return S_OK; }
    virtual HRESULT AppendKnownCategory( int category ) { return S_OK; }
    virtual HRESULT AddUserTasks(IObjectArray* poa) { return S_OK; }
    virtual HRESULT CommitList() { return S_OK; }
    virtual HRESULT DeleteList(LPCWSTR pszAppID) { return S_OK; }
};

enum TBPFLAG {
    TBPF_NOPROGRESS = 0,
    TBPF_INDETERMINATE = 0x1,
    TBPF_NORMAL = 0x2,
    TBPF_ERROR = 0x4,
    TBPF_PAUSED = 0x8,
};

enum THBMASK {
    THB_BITMAP = 0x1,
    THB_ICON = 0x2,
    THB_TOOLTIP = 0x4,
    THB_FLAGS = 0x8,
};
inline constexpr THBMASK operator|(THBMASK a, THBMASK b) {
    return static_cast<THBMASK>(static_cast<int>(a) | static_cast<int>(b));
}

enum THBFLAGS {
    THBF_ENABLED = 0,
    THBF_DISABLED = 0x1,
    THBF_DISMISSONCLICK = 0x2,
    THBF_NOBACKGROUND = 0x4,
    THBF_HIDDEN = 0x8,
    THBF_NONINTERACTIVE = 0x10,
};
inline constexpr THBFLAGS operator|(THBFLAGS a, THBFLAGS b) {
    return static_cast<THBFLAGS>(static_cast<int>(a) | static_cast<int>(b));
}

struct THUMBBUTTON {
    THBMASK dwMask;
    UINT iId;
    UINT iBitmap;
    HICON hIcon;
    WCHAR szTip[260];
    THBFLAGS dwFlags;
};
using LPTHUMBBUTTON = THUMBBUTTON*;

struct ITaskbarList3 : public IUnknown {
    virtual HRESULT HrInit() { return S_OK; }
    virtual HRESULT SetProgressValue(HWND hwnd, ULONGLONG ullCompleted, ULONGLONG ullTotal) { return S_OK; }
    virtual HRESULT SetProgressState(HWND hwnd, TBPFLAG tbpFlags) { return S_OK; }
    virtual HRESULT ThumbBarAddButtons(HWND hwnd, UINT cButtons, LPTHUMBBUTTON pButton) { return S_OK; }
    virtual HRESULT ThumbBarUpdateButtons(HWND hwnd, UINT cButtons, LPTHUMBBUTTON pButton) { return S_OK; }
    virtual HRESULT SetOverlayIcon(HWND hwnd, HICON hIcon, LPCWSTR pszDescription) { return S_OK; }
    virtual HRESULT SetThumbnailTooltip(HWND hwnd, LPCWSTR pszTip) { return S_OK; }
    virtual HRESULT SetThumbnailClip(HWND hwnd, RECT* prcClip) { return S_OK; }
};

struct IApplicationAssociationRegistrationUI : public IUnknown {
    virtual HRESULT LaunchAdvancedAssociationUI(LPCWSTR pszAppRegistryName) { return S_OK; }
};

// ============================================================================
// UIAutomation / MSAA / Accessibility Interfaces (for Accessibility.h / App.h)
// ============================================================================

using PROPERTYID = int;
using PATTERNID = int;
using EVENTID = int;
using DISPID = LONG;

struct ITypeInfo : public IUnknown {};
struct DISPPARAMS {};
struct EXCEPINFO {};

struct IDispatch : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* pctinfo) { if (pctinfo) *pctinfo = 0; return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT /*iTInfo*/, LCID /*lcid*/, ITypeInfo** ppTInfo) { if (ppTInfo) *ppTInfo = nullptr; return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID /*riid*/, LPOLESTR* /*rgszNames*/, UINT /*cNames*/, LCID /*lcid*/, DISPID* /*rgDispId*/) { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE Invoke(DISPID /*dispIdMember*/, REFIID /*riid*/, LCID /*lcid*/, WORD /*wFlags*/, DISPPARAMS* /*pDispParams*/, VARIANT* /*pVarResult*/, EXCEPINFO* /*pExcepInfo*/, UINT* /*puArgErr*/) { return E_NOTIMPL; }
};

struct IAccessible : public IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** ppdispParent) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_accChildCount(long* pcountChildren) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_accChild(VARIANT varChild, IDispatch** ppdispChild) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_accName(VARIANT varChild, BSTR* pszName) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_accValue(VARIANT varChild, BSTR* pszValue) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT varChild, BSTR* pszDescription) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_accRole(VARIANT varChild, VARIANT* pvarRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_accState(VARIANT varChild, VARIANT* pvarState) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT varChild, BSTR* pszHelp) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* pszHelpFile, VARIANT varChild, long* pidTopic) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT varChild, BSTR* pszKeyboardShortcut) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* pvarChild) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* pvarChildren) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT varChild, BSTR* pszDefaultAction) = 0;
    virtual HRESULT STDMETHODCALLTYPE accSelect(long flagsSelect, VARIANT varChild) = 0;
    virtual HRESULT STDMETHODCALLTYPE accLocation(long* pxLeft, long* pyTop, long* pcxWidth, long* pcyHeight, VARIANT varChild) = 0;
    virtual HRESULT STDMETHODCALLTYPE accNavigate(long navDir, VARIANT varStart, VARIANT* pvarEndUpAt) = 0;
    virtual HRESULT STDMETHODCALLTYPE accHitTest(long xLeft, long yTop, VARIANT* pvarChild) = 0;
    virtual HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT varChild) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_accName(VARIANT varChild, BSTR szName) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_accValue(VARIANT varChild, BSTR szValue) = 0;
};

inline constexpr GUID IID_IUnknown = __uuidof(IUnknown);
inline constexpr GUID IID_IDispatch = __uuidof(IDispatch);
inline constexpr GUID IID_IAccessible = __uuidof(IAccessible);

inline LRESULT LresultFromObject(REFIID /*riid*/, WPARAM /*wParam*/, LPUNKNOWN punk) {
    return reinterpret_cast<LRESULT>(punk);
}

// MSAA Roles
#define ROLE_SYSTEM_WINDOW 0x09
#define ROLE_SYSTEM_CLIENT 0x0a
#define ROLE_SYSTEM_MENUITEM 0x0c
#define ROLE_SYSTEM_ALERT 0x08
#define ROLE_SYSTEM_PANE 0x10
#define ROLE_SYSTEM_GROUPING 0x14
#define ROLE_SYSTEM_TOOLBAR 0x16
#define ROLE_SYSTEM_STATUSBAR 0x17
#define ROLE_SYSTEM_LIST 0x21
#define ROLE_SYSTEM_LISTITEM 0x22
#define ROLE_SYSTEM_PAGETAB 0x25
#define ROLE_SYSTEM_STATICTEXT 0x29
#define ROLE_SYSTEM_TEXT 0x2a
#define ROLE_SYSTEM_PUSHBUTTON 0x2b
#define ROLE_SYSTEM_CHECKBUTTON 0x2c
#define ROLE_SYSTEM_RADIOBUTTON 0x2d
#define ROLE_SYSTEM_COMBOBOX 0x2e
#define ROLE_SYSTEM_PROGRESSBAR 0x30
#define ROLE_SYSTEM_SLIDER 0x33
#define ROLE_SYSTEM_LINK 0x1e
#define ROLE_SYSTEM_PAGETABLIST 0x3c

// MSAA States
#define STATE_SYSTEM_UNAVAILABLE 0x00000001
#define STATE_SYSTEM_SELECTED 0x00000002
#define STATE_SYSTEM_FOCUSED 0x00000004
#define STATE_SYSTEM_CHECKED 0x00000010
#define STATE_SYSTEM_READONLY 0x00000040
#define STATE_SYSTEM_INVISIBLE 0x00008000
#define STATE_SYSTEM_OFFSCREEN 0x00010000
#define STATE_SYSTEM_FOCUSABLE 0x00100000

// MSAA Selection & Navigation Flags
#define SELFLAG_TAKEFOCUS 0x00000001
#define SELFLAG_TAKESELECTION 0x00000002
#define SELFLAG_EXTENDSELECTION 0x00000004
#define SELFLAG_ADDSELECTION 0x00000008
#define SELFLAG_REMOVESELECTION 0x00000010

#define NAVDIR_UP 0x00000001
#define NAVDIR_NEXT 0x00000005
#define NAVDIR_PREVIOUS 0x00000006
#define NAVDIR_FIRSTCHILD 0x00000007
#define NAVDIR_LASTCHILD 0x00000008

// UIA Properties
#define UIA_BoundingRectanglePropertyId 30001
#define UIA_ControlTypePropertyId 30003
#define UIA_LocalizedControlTypePropertyId 30004
#define UIA_NamePropertyId 30005
#define UIA_AcceleratorKeyPropertyId 30006
#define UIA_AccessKeyPropertyId 30007
#define UIA_HasKeyboardFocusPropertyId 30008
#define UIA_IsKeyboardFocusablePropertyId 30009
#define UIA_IsEnabledPropertyId 30010
#define UIA_AutomationIdPropertyId 30011
#define UIA_ClassNamePropertyId 30012
#define UIA_HelpTextPropertyId 30013
#define UIA_IsPasswordPropertyId 30019
#define UIA_NativeWindowHandlePropertyId 30020
#define UIA_ItemTypePropertyId 30021
#define UIA_IsOffscreenPropertyId 30022
#define UIA_ItemStatusPropertyId 30026
#define UIA_IsInvokePatternAvailablePropertyId 30031
#define UIA_IsRangeValuePatternAvailablePropertyId 30033
#define UIA_IsScrollPatternAvailablePropertyId 30034
#define UIA_IsScrollItemPatternAvailablePropertyId 30035
#define UIA_IsSelectionItemPatternAvailablePropertyId 30036
#define UIA_IsSelectionPatternAvailablePropertyId 30037
#define UIA_IsTogglePatternAvailablePropertyId 30041
#define UIA_IsValuePatternAvailablePropertyId 30043
#define UIA_ValueValuePropertyId 30045
#define UIA_ValueIsReadOnlyPropertyId 30046
#define UIA_RangeValueValuePropertyId 30047
#define UIA_RangeValueIsReadOnlyPropertyId 30048
#define UIA_RangeValueMinimumPropertyId 30049
#define UIA_RangeValueMaximumPropertyId 30050
#define UIA_RangeValueLargeChangePropertyId 30051
#define UIA_RangeValueSmallChangePropertyId 30052
#define UIA_ScrollHorizontalScrollPercentPropertyId 30053
#define UIA_ScrollHorizontalViewSizePropertyId 30054
#define UIA_ScrollVerticalScrollPercentPropertyId 30055
#define UIA_ScrollVerticalViewSizePropertyId 30056
#define UIA_ScrollHorizontallyScrollablePropertyId 30057
#define UIA_ScrollVerticallyScrollablePropertyId 30058
#define UIA_SelectionSelectionPropertyId 30059
#define UIA_SelectionCanSelectMultiplePropertyId 30060
#define UIA_SelectionIsSelectionRequiredPropertyId 30061
#define UIA_SelectionItemIsSelectedPropertyId 30079
#define UIA_SelectionItemSelectionContainerPropertyId 30080
#define UIA_ToggleToggleStatePropertyId 30086
#define UIA_AriaRolePropertyId 30101
#define UIA_AriaPropertiesPropertyId 30102
#define UIA_ControllerForPropertyId 30104
#define UIA_DescribedByPropertyId 30105
#define UIA_FlowsToPropertyId 30106
#define UIA_LiveSettingPropertyId 30135
#define UIA_FlowsFromPropertyId 30148
#define UIA_PositionInSetPropertyId 30152
#define UIA_SizeOfSetPropertyId 30153
#define UIA_LevelPropertyId 30154

// UIA Control Types
#define UIA_ButtonControlTypeId 50000
#define UIA_CheckBoxControlTypeId 50002
#define UIA_ComboBoxControlTypeId 50003
#define UIA_EditControlTypeId 50004
#define UIA_HyperlinkControlTypeId 50005
#define UIA_ListItemControlTypeId 50007
#define UIA_ListControlTypeId 50008
#define UIA_MenuItemControlTypeId 50011
#define UIA_ProgressBarControlTypeId 50012
#define UIA_RadioButtonControlTypeId 50013
#define UIA_SliderControlTypeId 50015
#define UIA_StatusBarControlTypeId 50017
#define UIA_TabControlTypeId 50018
#define UIA_TabItemControlTypeId 50019
#define UIA_TextControlTypeId 50020
#define UIA_ToolBarControlTypeId 50021
#define UIA_CustomControlTypeId 50025
#define UIA_GroupControlTypeId 50026
#define UIA_WindowControlTypeId 50032
#define UIA_PaneControlTypeId 50033

// UIA Pattern IDs
#define UIA_InvokePatternId 10000
#define UIA_SelectionPatternId 10001
#define UIA_ValuePatternId 10002
#define UIA_RangeValuePatternId 10003
#define UIA_ScrollPatternId 10004
#define UIA_SelectionItemPatternId 10010
#define UIA_TogglePatternId 10015
#define UIA_ScrollItemPatternId 10017

// UIA Events & Errors
#define UIA_StructureChangedEventId 20002
#define UIA_AutomationPropertyChangedEventId 20004
#define UIA_AutomationFocusChangedEventId 20005
#define UIA_Invoke_InvokedEventId 20009
#define UIA_SelectionItem_ElementAddedToSelectionEventId 20010
#define UIA_SelectionItem_ElementRemovedFromSelectionEventId 20011
#define UIA_SelectionItem_ElementSelectedEventId 20012
#define UIA_Selection_InvalidatedEventId 20013
#define UIA_LiveRegionChangedEventId 20024
#define UiaAppendRuntimeId 3
#define UIA_ScrollPatternNoScroll (-1.0)
#define UIA_E_ELEMENTNOTENABLED ((HRESULT)0x80040200L)
#define UIA_E_ELEMENTNOTAVAILABLE ((HRESULT)0x80040201L)
#define UIA_E_NOTSUPPORTED ((HRESULT)0x80040204L)
#define UIA_E_INVALIDOPERATION ((HRESULT)0x80131509L)

enum LiveSetting {
    Off = 0,
    Polite = 1,
    Assertive = 2,
};

enum ScrollAmount {
    ScrollAmount_LargeDecrement = 0,
    ScrollAmount_SmallDecrement = 1,
    ScrollAmount_NoAmount = 2,
    ScrollAmount_LargeIncrement = 3,
    ScrollAmount_SmallIncrement = 4,
};

enum ProviderOptions {
    ProviderOptions_ClientSideProvider = 0x1,
    ProviderOptions_ServerSideProvider = 0x2,
    ProviderOptions_NonClientAreaProvider = 0x4,
    ProviderOptions_OverrideProvider = 0x8,
    ProviderOptions_ProviderOwnsSetFocus = 0x10,
    ProviderOptions_UseComThreading = 0x20,
};
inline constexpr ProviderOptions operator|(ProviderOptions a, ProviderOptions b) {
    return static_cast<ProviderOptions>(static_cast<int>(a) | static_cast<int>(b));
}

enum NavigateDirection {
    NavigateDirection_Parent = 0,
    NavigateDirection_NextSibling = 1,
    NavigateDirection_PreviousSibling = 2,
    NavigateDirection_FirstChild = 3,
    NavigateDirection_LastChild = 4,
};

enum StructureChangeType {
    StructureChangeType_ChildAdded = 0,
    StructureChangeType_ChildRemoved = 1,
    StructureChangeType_ChildrenInvalidated = 2,
    StructureChangeType_ChildrenBulkAdded = 3,
    StructureChangeType_ChildrenBulkRemoved = 4,
    StructureChangeType_ChildrenReordered = 5,
};

enum ToggleState {
    ToggleState_Off = 0,
    ToggleState_On = 1,
    ToggleState_Indeterminate = 2,
};

struct UiaRect {
    double left;
    double top;
    double width;
    double height;
};

struct SAFEARRAYBOUND {
    ULONG cElements;
    LONG lLbound;
};

struct SAFEARRAY {
    USHORT cDims = 1;
    USHORT fFeatures = 0;
    ULONG cbElements = 0;
    ULONG cLocks = 0;
    PVOID pvData = nullptr;
    SAFEARRAYBOUND rgsabound[1]{};
    VARTYPE vt = VT_EMPTY;
    std::vector<uint8_t> m_storage;
};

#define VARIANT_TRUE ((SHORT)-1)
#define VARIANT_FALSE ((SHORT)0)

inline size_t __LinuxVarTypeSize(VARTYPE vt) {
    switch (vt) {
        case VT_I4: case VT_UI4: case VT_R4: return 4;
        case VT_R8: case VT_I8: case VT_UI8: return 8;
        case VT_I2: case VT_UI2: case VT_BOOL: return 2;
        case VT_UI1: return 1;
        case VT_UNKNOWN: case VT_DISPATCH: case VT_BSTR: return sizeof(void*);
        default: return 8;
    }
}

inline SAFEARRAY* SafeArrayCreate(VARTYPE vt, UINT cDims, SAFEARRAYBOUND* rgsabound) {
    if (cDims != 1 || !rgsabound) return nullptr;
    auto* sa = new SAFEARRAY();
    sa->cDims = 1;
    sa->vt = vt;
    sa->cbElements = static_cast<ULONG>(__LinuxVarTypeSize(vt));
    sa->rgsabound[0] = *rgsabound;
    sa->m_storage.resize(static_cast<size_t>(rgsabound->cElements) * sa->cbElements, 0);
    sa->pvData = sa->m_storage.empty() ? nullptr : sa->m_storage.data();
    return sa;
}
inline HRESULT SafeArrayAccessData(SAFEARRAY* psa, void** ppvData) {
    if (!psa || !ppvData) return E_INVALIDARG;
    *ppvData = psa->m_storage.empty() ? nullptr : psa->m_storage.data();
    return S_OK;
}
inline HRESULT SafeArrayUnaccessData(SAFEARRAY* psa) {
    return psa ? S_OK : E_INVALIDARG;
}
inline HRESULT SafeArrayDestroy(SAFEARRAY* psa) {
    if (!psa) return S_OK;
    if (psa->vt == VT_UNKNOWN || psa->vt == VT_DISPATCH) {
        auto** ptrs = reinterpret_cast<IUnknown**>(psa->m_storage.data());
        for (ULONG i = 0; i < psa->rgsabound[0].cElements; ++i) {
            if (ptrs[i]) ptrs[i]->Release();
        }
    } else if (psa->vt == VT_BSTR) {
        auto** bstrs = reinterpret_cast<BSTR*>(psa->m_storage.data());
        for (ULONG i = 0; i < psa->rgsabound[0].cElements; ++i) {
            if (bstrs[i]) std::free(bstrs[i]);
        }
    }
    delete psa;
    return S_OK;
}

inline void VariantInit(VARIANT* pvarg) {
    if (pvarg) std::memset(pvarg, 0, sizeof(VARIANT));
}
inline HRESULT VariantClear(VARIANT* pvarg) {
    if (pvarg) {
        if (pvarg->vt == VT_BSTR && pvarg->bstrVal) {
            std::free(pvarg->bstrVal);
        } else if ((pvarg->vt == VT_UNKNOWN || pvarg->vt == VT_DISPATCH) && pvarg->punkVal) {
            pvarg->punkVal->Release();
        } else if ((pvarg->vt & VT_ARRAY) && pvarg->parray) {
            SafeArrayDestroy(pvarg->parray);
        }
        std::memset(pvarg, 0, sizeof(VARIANT));
    }
    return S_OK;
}
inline BSTR SysAllocString(const WCHAR* psz) {
    if (!psz) return nullptr;
    size_t len = std::wcslen(psz);
    BSTR b = static_cast<BSTR>(std::malloc((len + 1) * sizeof(WCHAR)));
    if (b) std::memcpy(b, psz, (len + 1) * sizeof(WCHAR));
    return b;
}
inline BSTR SysAllocStringLen(const WCHAR* strIn, UINT ui) {
    BSTR b = static_cast<BSTR>(std::malloc((ui + 1) * sizeof(WCHAR)));
    if (b) {
        if (strIn && ui > 0) std::memcpy(b, strIn, ui * sizeof(WCHAR));
        b[ui] = L'\0';
    }
    return b;
}

struct IRawElementProviderSimple : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) = 0;
};
struct IRawElementProviderFragmentRoot;
struct IRawElementProviderFragment : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFocus() = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) = 0;
};
struct IRawElementProviderFragmentRoot : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** pRetVal) = 0;
};
struct IRawElementProviderAdviseEvents : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE AdviseEventAdded(EVENTID eventId, SAFEARRAY* propertyIDs) = 0;
    virtual HRESULT STDMETHODCALLTYPE AdviseEventRemoved(EVENTID eventId, SAFEARRAY* propertyIDs) = 0;
};
struct IInvokeProvider : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke() = 0;
};
struct IValueProvider : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR val) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Value(BSTR* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* pRetVal) = 0;
};
struct IRangeValueProvider : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetValue(double val) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Value(double* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Maximum(double* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Minimum(double* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LargeChange(double* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SmallChange(double* pRetVal) = 0;
};
struct IToggleProvider : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Toggle() = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* pRetVal) = 0;
};
struct ISelectionProvider : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* pRetVal) = 0;
};
struct ISelectionItemProvider : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Select() = 0;
    virtual HRESULT STDMETHODCALLTYPE AddToSelection() = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveFromSelection() = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** pRetVal) = 0;
};
struct IScrollItemProvider : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE ScrollIntoView() = 0;
};
struct IScrollProvider : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Scroll(ScrollAmount horizontalAmount, ScrollAmount verticalAmount) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetScrollPercent(double horizontalPercent, double verticalPercent) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HorizontalScrollPercent(double* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_VerticalScrollPercent(double* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HorizontalViewSize(double* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_VerticalViewSize(double* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HorizontallyScrollable(BOOL* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_VerticallyScrollable(BOOL* pRetVal) = 0;
};

inline LRESULT UiaReturnRawElementProvider(HWND /*hwnd*/, WPARAM /*wParam*/, LPARAM /*lParam*/, IRawElementProviderSimple* el) {
    return reinterpret_cast<LRESULT>(el);
}
inline HRESULT UiaHostProviderFromHwnd(HWND /*hwnd*/, IRawElementProviderSimple** ppProvider) {
    if (ppProvider) *ppProvider = nullptr;
    return E_NOTIMPL;
}
inline HRESULT UiaDisconnectProvider(IRawElementProviderSimple* /*pProvider*/) { return S_OK; }
inline HRESULT UiaRaiseAutomationEvent(IRawElementProviderSimple* /*pProvider*/, EVENTID /*id*/) { return S_OK; }
inline HRESULT UiaRaiseAutomationPropertyChangedEvent(IRawElementProviderSimple* /*pProvider*/, PROPERTYID /*id*/, VARIANT /*oldValue*/, VARIANT /*newValue*/) { return S_OK; }
inline HRESULT UiaRaiseStructureChangedEvent(IRawElementProviderSimple* /*pProvider*/, StructureChangeType /*structureChangeType*/, int* /*pRuntimeId*/, int /*cRuntimeIdLen*/) { return S_OK; }
inline BOOL UiaClientsAreListening() { return FALSE; }

#define EVENT_SYSTEM_ALERT 0x0002
#define EVENT_OBJECT_REORDER 0x8004
#define EVENT_OBJECT_FOCUS 0x8005
#define EVENT_OBJECT_STATECHANGE 0x800A
#define EVENT_OBJECT_LOCATIONCHANGE 0x800B
#define EVENT_OBJECT_NAMECHANGE 0x800C
#define EVENT_OBJECT_VALUECHANGE 0x800E
#define OBJID_CLIENT ((LONG)0xFFFFFFFC)
#define CHILDID_SELF 0
inline void NotifyWinEvent(DWORD /*event*/, HWND /*hwnd*/, LONG /*idObject*/, LONG /*idChild*/) {}

struct EventRegistrationToken {
    int64_t value = 0;
};

namespace ABI {
namespace Windows {
namespace Media {

enum SystemMediaTransportControlsButton {
    SystemMediaTransportControlsButton_Play = 0,
    SystemMediaTransportControlsButton_Pause = 1,
    SystemMediaTransportControlsButton_Stop = 2,
    SystemMediaTransportControlsButton_Record = 3,
    SystemMediaTransportControlsButton_FastForward = 4,
    SystemMediaTransportControlsButton_Rewind = 5,
    SystemMediaTransportControlsButton_Next = 6,
    SystemMediaTransportControlsButton_Previous = 7,
    SystemMediaTransportControlsButton_ChannelUp = 8,
    SystemMediaTransportControlsButton_ChannelDown = 9,
};

struct ISystemMediaTransportControls : public IInspectable {};
struct ISystemMediaTransportControls2 : public IInspectable {};
struct ISystemMediaTransportControlsDisplayUpdater : public IInspectable {};

} // namespace Media
} // namespace Windows
} // namespace ABI

// ============================================================================
// WinHTTP & BCrypt & WinTrust Constants and Types
// ============================================================================

#define INTERNET_DEFAULT_HTTPS_PORT 443
#define INTERNET_SCHEME_HTTP 1
#define INTERNET_SCHEME_HTTPS 2
#define ERROR_WINHTTP_TIMEOUT 12002L
#define ERROR_WINHTTP_OPERATION_CANCELLED 12017L
#ifndef ERROR_NOT_READY
#define ERROR_NOT_READY 21L
#endif
#ifndef ERROR_WRITE_PROTECT
#define ERROR_WRITE_PROTECT 19L
#endif
#ifndef ERROR_HANDLE_DISK_FULL
#define ERROR_HANDLE_DISK_FULL 39L
#endif
#ifndef ERROR_READ_FAULT
#define ERROR_READ_FAULT 30L
#endif

struct URL_COMPONENTS {
    DWORD dwStructSize;
    LPWSTR lpszScheme;
    DWORD dwSchemeLength;
    int nScheme;
    LPWSTR lpszHostName;
    DWORD dwHostNameLength;
    WORD nPort;
    LPWSTR lpszUserName;
    DWORD dwUserNameLength;
    LPWSTR lpszPassword;
    DWORD dwPasswordLength;
    LPWSTR lpszUrlPath;
    DWORD dwUrlPathLength;
    LPWSTR lpszExtraInfo;
    DWORD dwExtraInfoLength;
};
using LPURL_COMPONENTS = URL_COMPONENTS*;

BOOL WinHttpCrackUrl(LPCWSTR pwszUrl, DWORD dwUrlLength, DWORD dwFlags, LPURL_COMPONENTS lpUrlComponents);

#define BCRYPT_SHA256_ALGORITHM L"SHA256"
#define BCRYPT_ECDSA_P256_ALGORITHM L"ECDSA_P256"
#define BCRYPT_HASH_LENGTH L"HashDigestLength"
#define BCRYPT_OBJECT_LENGTH L"ObjectLength"
#define BCRYPT_ECCPUBLIC_BLOB L"ECCPUBLICBLOB"
#define BCRYPT_ECDSA_PUBLIC_P256_MAGIC 0x31534345

struct BCRYPT_ECCKEY_BLOB {
    ULONG dwMagic;
    ULONG cbKey;
};

NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE* phAlgorithm, LPCWSTR pszAlgId, LPCWSTR pszImplementation, ULONG dwFlags);
NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE hAlgorithm, ULONG dwFlags);
NTSTATUS BCryptGetProperty(BCRYPT_HANDLE hObject, LPCWSTR pszProperty, PUCHAR pbOutput, ULONG cbOutput, ULONG* pcbResult, ULONG dwFlags);
NTSTATUS BCryptCreateHash(BCRYPT_ALG_HANDLE hAlgorithm, BCRYPT_HASH_HANDLE* phHash, PUCHAR pbHashObject, ULONG cbHashObject, PUCHAR pbSecret, ULONG cbSecret, ULONG dwFlags);
NTSTATUS BCryptHashData(BCRYPT_HASH_HANDLE hHash, PUCHAR pbInput, ULONG cbInput, ULONG dwFlags);
NTSTATUS BCryptFinishHash(BCRYPT_HASH_HANDLE hHash, PUCHAR pbOutput, ULONG cbOutput, ULONG dwFlags);
NTSTATUS BCryptDestroyHash(BCRYPT_HASH_HANDLE hHash);
NTSTATUS BCryptImportKeyPair(BCRYPT_ALG_HANDLE hAlgorithm, BCRYPT_KEY_HANDLE hImportKey, LPCWSTR pszBlobType, BCRYPT_KEY_HANDLE* phKey, PUCHAR pbInput, ULONG cbInput, ULONG dwFlags);
NTSTATUS BCryptVerifySignature(BCRYPT_KEY_HANDLE hKey, void* pPaddingInfo, PUCHAR pbHash, ULONG cbHash, PUCHAR pbSignature, ULONG cbSignature, ULONG dwFlags);
NTSTATUS BCryptDestroyKey(BCRYPT_KEY_HANDLE hKey);

#define CRYPT_STRING_BASE64 0x00000001
#define CRYPT_STRING_BASE64_ANY 0x00000006
#define CRYPT_STRING_HEXRAW 0x0000000c
#define CRYPT_STRING_NOCRLF 0x40000000
BOOL CryptStringToBinaryW(LPCWSTR pszString, DWORD cchString, DWORD dwFlags, BYTE* pbBinary, DWORD* pcbBinary, DWORD* pdwSkip, DWORD* pdwFlags);
BOOL CryptStringToBinaryA(LPCSTR pszString, DWORD cchString, DWORD dwFlags, BYTE* pbBinary, DWORD* pcbBinary, DWORD* pdwSkip, DWORD* pdwFlags);
BOOL CryptBinaryToStringW(const BYTE* pbBinary, DWORD cbBinary, DWORD dwFlags, LPWSTR pszString, DWORD* pcchString);

// WinTrust / Crypt32 / PE Header types for Updater.cpp
#define WTD_UI_NONE 2
#define WTD_REVOKE_NONE 0
#define WTD_CHOICE_FILE 1
#define WTD_STATEACTION_VERIFY 1
#define WTD_STATEACTION_CLOSE 2
#define WTD_SAFER_FLAG 0x00000100
#define TRUST_E_NOSIGNATURE ((HRESULT)0x800B0100L)
inline constexpr GUID WINTRUST_ACTION_GENERIC_VERIFY_V2 = {0x00aac56b, 0xcd44, 0x11d0, {0x8c, 0xc2, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee}};

struct WINTRUST_FILE_INFO {
    DWORD cbStruct;
    LPCWSTR pcwszFilePath;
    HANDLE hFile;
    GUID* pgKnownSubject;
};

struct WINTRUST_DATA {
    DWORD cbStruct;
    LPVOID pPolicyCallbackData;
    LPVOID pSIPClientData;
    DWORD dwUIChoice;
    DWORD fdwRevocationChecks;
    DWORD dwUnionChoice;
    WINTRUST_FILE_INFO* pFile;
    DWORD dwStateAction;
    HANDLE hWVTStateData;
    WCHAR* pwszURLReference;
    DWORD dwProvFlags;
    DWORD dwUIContext;
};

inline LONG WinVerifyTrust(HWND /*hwnd*/, GUID* /*pgActionID*/, LPVOID /*pWVTData*/) { return 0; }

using HCERTSTORE = void*;
using HCRYPTMSG = void*;
#define CERT_QUERY_OBJECT_FILE 1
#define CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED (1 << 10)
#define CERT_QUERY_FORMAT_FLAG_BINARY (1 << 1)
#define CMSG_SIGNER_INFO_PARAM 6
#define X509_ASN_ENCODING 0x00000001
#define PKCS_7_ASN_ENCODING 0x00010000
#define CERT_FIND_SUBJECT_CERT (11 << 16)

struct CRYPT_INTEGER_BLOB {
    DWORD cbData;
    BYTE* pbData;
};
using CERT_NAME_BLOB = CRYPT_INTEGER_BLOB;

struct CMSG_SIGNER_INFO {
    DWORD dwVersion;
    CERT_NAME_BLOB Issuer;
    CRYPT_INTEGER_BLOB SerialNumber;
};

struct CERT_INFO {
    DWORD dwVersion;
    CRYPT_INTEGER_BLOB SerialNumber;
    CERT_NAME_BLOB Issuer;
};

struct CERT_CONTEXT {
    DWORD dwCertEncodingType;
    BYTE* pbCertEncoded;
    DWORD cbCertEncoded;
    CERT_INFO* pCertInfo;
    HCERTSTORE hCertStore;
};
using PCCERT_CONTEXT = const CERT_CONTEXT*;

inline BOOL CryptQueryObject(DWORD, const void*, DWORD, DWORD, DWORD, DWORD*, DWORD*, DWORD*, HCERTSTORE*, HCRYPTMSG*, const void**) { return FALSE; }
inline BOOL CertCloseStore(HCERTSTORE, DWORD) { return TRUE; }
inline BOOL CryptMsgClose(HCRYPTMSG) { return TRUE; }
inline BOOL CryptMsgGetParam(HCRYPTMSG, DWORD, DWORD, void*, DWORD*) { return FALSE; }
inline PCCERT_CONTEXT CertFindCertificateInStore(HCERTSTORE, DWORD, DWORD, DWORD, const void*, PCCERT_CONTEXT) { return nullptr; }
inline BOOL CertFreeCertificateContext(PCCERT_CONTEXT) { return TRUE; }

#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20b
#define IMAGE_SUBSYSTEM_WINDOWS_GUI 2
#define IMAGE_SUBSYSTEM_WINDOWS_CUI 3

struct IMAGE_DOS_HEADER {
    WORD e_magic;
    WORD e_cblp;
    WORD e_cp;
    WORD e_crlc;
    WORD e_cparhdr;
    WORD e_minalloc;
    WORD e_maxalloc;
    WORD e_ss;
    WORD e_sp;
    WORD e_csum;
    WORD e_ip;
    WORD e_cs;
    WORD e_lfarlc;
    WORD e_ovno;
    WORD e_res[4];
    WORD e_oemid;
    WORD e_oeminfo;
    WORD e_res2[10];
    LONG e_lfanew;
};

struct IMAGE_FILE_HEADER {
    WORD Machine;
    WORD NumberOfSections;
    DWORD TimeDateStamp;
    DWORD PointerToSymbolTable;
    DWORD NumberOfSymbols;
    WORD SizeOfOptionalHeader;
    WORD Characteristics;
};

struct IMAGE_OPTIONAL_HEADER64 {
    WORD Magic;
    BYTE MajorLinkerVersion;
    BYTE MinorLinkerVersion;
    DWORD SizeOfCode;
    DWORD SizeOfInitializedData;
    DWORD SizeOfUninitializedData;
    DWORD AddressOfEntryPoint;
    DWORD BaseOfCode;
    ULONGLONG ImageBase;
    DWORD SectionAlignment;
    DWORD FileAlignment;
    WORD MajorOperatingSystemVersion;
    WORD MinorOperatingSystemVersion;
    WORD MajorImageVersion;
    WORD MinorImageVersion;
    WORD MajorSubsystemVersion;
    WORD MinorSubsystemVersion;
    DWORD Win32VersionValue;
    DWORD SizeOfImage;
    DWORD SizeOfHeaders;
    DWORD CheckSum;
    WORD Subsystem;
    WORD DllCharacteristics;
};

#endif // !defined(_WIN32)
