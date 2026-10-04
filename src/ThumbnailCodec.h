#pragma once

#include "AppIncludes.h"
#include <wincodec.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

namespace Aperture {

#pragma pack(push, 1)
struct ThumbnailDiskHeader {
    uint32_t magic = 0x5448554D;
    uint32_t version = 1, width = 0, height = 0, stride = 0;
    double timestamp = 0.0;
    uint32_t pixelBytes = 0;
};
#pragma pack(pop)
static_assert(sizeof(ThumbnailDiskHeader) == 32, "Stable legacy thumbnail header");

// v1 = legacy BGRA, v2 = PNG, v3 = JPEG. Pixels are always BGRA in memory.
// This codec is independent of the player so real cache files can be benchmarked
// without opening media, constructing a window, or touching the user's profile.
namespace ThumbnailCodec {
constexpr float kJpegQuality = 0.80f;
constexpr size_t kMaxPixelBytes = 1920u * 1080u * 4u;

struct ComScope {
    bool owned = false;
    ComScope() {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // Hold our own COM reference even on the UI's STA. The thread-local
        // factory may outlive the caller's final CoUninitialize at shutdown.
        if (result == RPC_E_CHANGED_MODE) result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        owned = SUCCEEDED(result);
    }
    ~ComScope() { if (owned) CoUninitialize(); }
};

struct ThreadCodec {
    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    ThreadCodec() {
        if (com.owned && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            factory.Reset();
    }
};
inline IWICImagingFactory* Factory() {
    thread_local ThreadCodec codec;
    return codec.factory.Get();
}

inline bool EncodeImage(const uint8_t* pixels, UINT w, UINT h, bool jpeg,
                        std::vector<uint8_t>& out, float quality = kJpegQuality) {
    out.clear();
    if (!pixels || !w || !h || w > 1920 || h > 1080) return false;
    auto* factory = Factory();
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    if (!factory ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, &options))) return false;
    if (jpeg) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value{}; value.vt = VT_R4; value.fltVal = quality;
        if (!options || FAILED(options->Write(1, &option, &value))) return false;
        // Saturated graphics and small subtitle text need full chroma resolution.
        option.pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
        value.vt = VT_UI1; value.bVal = WICJpegYCrCbSubsampling444;
        if (FAILED(options->Write(1, &option, &value))) return false;
    }
    if (FAILED(frame->Initialize(options.Get())) || FAILED(frame->SetSize(w, h))) return false;
    GUID format = jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) ||
        !IsEqualGUID(format, jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA)) return false;
    std::vector<uint8_t> bgr;
    UINT stride = w * 4;
    const uint8_t* source = pixels;
    if (jpeg) {
        bgr.resize(static_cast<size_t>(w) * h * 3);
        for (size_t i = 0, j = 0; i < bgr.size(); i += 3, j += 4) std::memcpy(bgr.data() + i, pixels + j, 3);
        source = bgr.data(); stride = w * 3;
    }
    if (FAILED(frame->WritePixels(h, stride, stride * h, const_cast<BYTE*>(source))) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || !stat.cbSize.QuadPart || stat.cbSize.QuadPart > kMaxPixelBytes) return false;
    out.resize(static_cast<size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{}; ULONG read = 0;
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)) ||
        FAILED(stream->Read(out.data(), static_cast<ULONG>(out.size()), &read)) || read != out.size()) { out.clear(); return false; }
    return true;
}

inline bool Decode(const std::string& bytes, ThumbnailDiskHeader& header, std::vector<uint8_t>& pixels) {
    pixels.clear();
    if (bytes.size() < sizeof(header)) return false;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != 0x5448554D || header.version < 1 || header.version > 3 ||
        !header.width || header.width > 1920 || !header.height || header.height > 1080 ||
        header.stride != header.width * 4 || !std::isfinite(header.timestamp) || header.timestamp < 0 ||
        !header.pixelBytes || header.pixelBytes > kMaxPixelBytes || bytes.size() != sizeof(header) + header.pixelBytes) return false;
    const size_t count = static_cast<size_t>(header.width) * header.height * 4;
    if (header.version == 1) {
        if (header.pixelBytes != count) return false;
        pixels.assign(bytes.begin() + sizeof(header), bytes.end());
        return true;
    }
    auto* factory = Factory();
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (!factory ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data() + sizeof(header))), header.pixelBytes)) ||
        FAILED(factory->CreateDecoder(header.version == 3 ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &decoder)) ||
        FAILED(decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand)) || FAILED(decoder->GetFrame(0, &frame))) return false;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w != header.width || h != header.height) return false;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    pixels.resize(count);
    if (FAILED(converter->CopyPixels(nullptr, header.stride, static_cast<UINT>(count), pixels.data()))) { pixels.clear(); return false; }
    return true;
}

// Sample at most 512 pixels. Flat graphics and limited palettes are worth a
// lossless PNG attempt; natural frames keep the single JPEG encoding path.
inline bool MayCompressBetterAsPng(const std::vector<uint8_t>& pixels, int w, int h) {
    uint32_t palette[32]{};
    size_t colors = 0, repeated = 0, probes = 0;
    bool paletteFull = false;
    for (int row = 0; row < std::min(h, 16); ++row) {
        const int y = row * h / std::min(h, 16);
        for (int column = 0; column < std::min(w, 16); ++column) {
            const int x = column * w / std::min(w, 16);
            const size_t index = (static_cast<size_t>(y) * w + x) * 4;
            const size_t neighbor = x + 1 < w ? index + 4 : index;
            repeated += std::memcmp(pixels.data() + index, pixels.data() + neighbor, 3) == 0;
            ++probes;
            if (paletteFull) continue;
            for (size_t offset : {index, neighbor}) {
                const uint32_t rgb = uint32_t(pixels[offset]) | (uint32_t(pixels[offset + 1]) << 8) |
                                     (uint32_t(pixels[offset + 2]) << 16);
                if (std::find(palette, palette + colors, rgb) != palette + colors) continue;
                if (colors == 32) { paletteFull = true; break; }
                palette[colors++] = rgb;
            }
        }
    }
    return !paletteFull || repeated * 10 >= probes * 9;
}

inline bool Encode(const std::vector<uint8_t>& pixels, int w, int h, double time, std::string& out) {
    out.clear();
    if (w <= 0 || h <= 0 || w > 1920 || h > 1080 || pixels.size() != static_cast<size_t>(w) * h * 4 ||
        !std::isfinite(time) || time < 0) return false;
    bool opaque = true;
    for (size_t i = 3; i < pixels.size(); i += 4) { if (pixels[i] != 255) { opaque = false; break; } }
    std::vector<uint8_t> encoded;
    bool jpeg = opaque && EncodeImage(pixels.data(), w, h, true, encoded);
    // Keep JPEG quality and full chroma resolution. For graphics, retain a
    // lossless PNG only when it actually occupies less space.
    if (jpeg && MayCompressBetterAsPng(pixels, w, h)) {
        std::vector<uint8_t> png;
        if (EncodeImage(pixels.data(), w, h, false, png) && png.size() < encoded.size()) {
            encoded.swap(png);
            jpeg = false;
        }
    } else if (!jpeg && !EncodeImage(pixels.data(), w, h, false, encoded)) return false;
    ThumbnailDiskHeader header;
    header.version = jpeg ? 3 : 2; header.width = w; header.height = h;
    header.stride = w * 4; header.timestamp = time; header.pixelBytes = static_cast<uint32_t>(encoded.size());
    out.resize(sizeof(header) + encoded.size());
    std::memcpy(out.data(), &header, sizeof(header));
    std::memcpy(out.data() + sizeof(header), encoded.data(), encoded.size());
    return true;
}

enum class UpgradeResult { Converted, AlreadyOptimal, Failed };

inline UpgradeResult TryUpgrade(const std::string& oldBytes, std::string& newBytes) {
    newBytes.clear();
    ThumbnailDiskHeader header{}; std::vector<uint8_t> pixels;
    if (!Decode(oldBytes, header, pixels)) return UpgradeResult::Failed;
    if (header.version == 3) return UpgradeResult::AlreadyOptimal;
    if (!Encode(pixels, header.width, header.height, header.timestamp, newBytes)) return UpgradeResult::Failed;
    if (newBytes.size() >= oldBytes.size()) {
        newBytes.clear();
        return UpgradeResult::AlreadyOptimal;
    }
    ThumbnailDiskHeader check{}; std::vector<uint8_t> decoded;
    return Decode(newBytes, check, decoded) ? UpgradeResult::Converted : UpgradeResult::Failed;
}

inline bool Upgrade(const std::string& oldBytes, std::string& newBytes) {
    return TryUpgrade(oldBytes, newBytes) == UpgradeResult::Converted;
}
} // namespace ThumbnailCodec
} // namespace Aperture
