#include "MpvPlayer.h"
#include <iostream>

using namespace Aperture;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Predicate> static void Until(Predicate predicate, const char* message) {
    const auto deadline = GetTickCount64() + 10000;
    do { if (predicate()) return; Sleep(10); } while (GetTickCount64() < deadline);
    throw std::runtime_error(message);
}
int wmain(int argc, wchar_t** argv) {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        Check(argc == 2 && FileExists(argv[1]), "Supply the generated local video fixture");
        Microsoft::WRL::ComPtr<ID3D11Device> d3d;
        Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)), "WARP initialization failed");
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi; Check(SUCCEEDED(d3d.As(&dxgi)), "DXGI binding failed");
        Microsoft::WRL::ComPtr<ID2D1Factory1> factory;
        Check(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))), "D2D factory failed");
        Microsoft::WRL::ComPtr<ID2D1Device> firstDevice, secondDevice;
        Check(SUCCEEDED(factory->CreateDevice(dxgi.Get(), &firstDevice)), "First D2D device failed");
        Check(SUCCEEDED(factory->CreateDevice(dxgi.Get(), &secondDevice)), "Second D2D device failed");
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> first, second;
        Check(SUCCEEDED(firstDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &first)), "First context failed");
        Check(SUCCEEDED(secondDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &second)), "Second context failed");
        MpvPreviewPlayer preview;
        Check(preview.Initialize(first.Get(), nullptr), "Actual preview profile initialization failed");
        preview.SetPreviewOutputSize(640, 360);
        Check(!preview.IsSeekbarActive() && !preview.HasSeekbarThumbnail(), "Idle preview allocated active seekbar state");
        preview.StartPreview(argv[1], 1);
        Until([&] { preview.UpdateFrame(first.Get()); return preview.GetPreviewBitmap() != nullptr; }, "Hover pipeline produced no decoded bitmap");
        const auto size = preview.GetPreviewBitmap()->GetPixelSize();
        Check(size.width == 640 && size.height == 360, "Hover bitmap dimensions changed");
        preview.StopPreview();
        preview.RequestSeekbarThumbnail(argv[1], 2, 45, 7, first.Get());
        Until([&] { preview.UpdateFrame(first.Get()); return preview.HasSeekbarThumbnail(); }, "Dormant seekbar extraction produced no thumbnail");
        unsigned heldUpdates = 0;
        const auto holdUntil = GetTickCount64() + 1000;
        do { if (preview.UpdateFrame(first.Get())) ++heldUpdates; Sleep(10); } while (GetTickCount64() < holdUntil);
        std::cout << "BASELINE stationary_hold_bitmap_updates=" << heldUpdates << "\n";
        preview.BindDeviceContext(second.Get());
        Check(!preview.HasSeekbarThumbnail(), "Seekbar bitmap survived a different D2D device");
        preview.RequestSeekbarThumbnail(argv[1], 2, 45, 7, second.Get());
        Until([&] { preview.UpdateFrame(second.Get()); return preview.HasSeekbarThumbnail(); }, "Cached thumbnail failed to upload on the new device");
        preview.Shutdown();
        MpvHubExtractor extractor;
        Check(extractor.Begin(argv[1], 1), "Actual hub extractor did not start");
        PreviewThumbnailFrame frame;
        Until([&] { extractor.Pump(); return extractor.TakeResult(frame); }, "Hub extractor produced no frame");
        Check(frame.width == 320 && frame.height == 180 && frame.bgraPixels.size() == 320 * 180 * 4, "Hub frame is incomplete");
        Check(std::any_of(frame.bgraPixels.begin(), frame.bgraPixels.end(), [](uint8_t pixel) { return pixel != 0; }), "Hub frame was empty");
        extractor.Shutdown();
        std::cout << "PASS actual_hover_decode=true dormant_seekbar_decode=true new_device_cache_upload=true actual_hub_decode=true\n";
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << "\n";
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
}
