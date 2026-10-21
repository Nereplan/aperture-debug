#include "MpvPlayer.h"
#include <iostream>

using namespace Aperture;

namespace Aperture {
// Count real decoder renders and loads without production instrumentation.
struct MpvPreviewTestAccess {
    inline static decltype(&mpv_render_context_render) render = nullptr;
    inline static decltype(&mpv_command) command = nullptr;
    inline static unsigned renders = 0, loads = 0;
    static int Render(mpv_render_context* context, mpv_render_param* params) {
        ++renders; return render(context, params);
    }
    static int Command(mpv_handle* handle, const char** args) {
        if (args && args[0] && std::strcmp(args[0], "loadfile") == 0) ++loads;
        return command(handle, args);
    }
    static void Instrument(MpvPreviewPlayer& player) {
        render = player.fn_mpv_render_context_render; command = player.fn_mpv_command;
        player.fn_mpv_render_context_render = &Render; player.fn_mpv_command = &Command;
        renders = loads = 0;
    }
    static bool GraphicsReleased(const MpvPreviewPlayer& player) { return !player.previewBitmap && !player.bitmapDevice; }
    static bool Released(const MpvPreviewPlayer& player) {
        return !player.mpv && !player.renderCtx && !player.hMpvDll && !player.previewBitmap &&
            !player.bitmapDevice && player.pixelBuffer.empty();
    }
};
}

static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Predicate> static void Until(Predicate predicate, const char* message) {
    const auto deadline = GetTickCount64() + 10000;
    do { if (predicate()) return; Sleep(10); } while (GetTickCount64() < deadline);
    throw std::runtime_error(message);
}
static unsigned Hold(MpvPreviewPlayer& preview, ID2D1DeviceContext* context, DWORD milliseconds, unsigned* updates = nullptr) {
    const auto before = MpvPreviewTestAccess::renders;
    const auto deadline = GetTickCount64() + milliseconds;
    do { if (preview.UpdateFrame(context) && updates) ++*updates; Sleep(10); } while (GetTickCount64() < deadline);
    return MpvPreviewTestAccess::renders - before;
}
int wmain(int argc, wchar_t** argv) {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        Check(argc == 2 && FileExists(argv[1]), "Supply the generated local video fixture");
        ComPtr<ID3D11Device> d3d;
        Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)), "WARP initialization failed");
        ComPtr<IDXGIDevice> dxgi; Check(SUCCEEDED(d3d.As(&dxgi)), "DXGI binding failed");
        ComPtr<ID2D1Factory1> factory;
        Check(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))), "D2D factory failed");
        ComPtr<ID2D1Device> firstDevice, secondDevice;
        Check(SUCCEEDED(factory->CreateDevice(dxgi.Get(), &firstDevice)), "First D2D device failed");
        Check(SUCCEEDED(factory->CreateDevice(dxgi.Get(), &secondDevice)), "Second D2D device failed");
        ComPtr<ID2D1DeviceContext> first, second;
        Check(SUCCEEDED(firstDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &first)), "First context failed");
        Check(SUCCEEDED(secondDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &second)), "Second context failed");
        const auto key = MakeHubThumbnailCacheKey(argv[1], 0, 0);
        const auto cachedPath = GetCacheDir(CacheClass::Thumbnails) + L"\\" + key;
        Check(!FileExists(cachedPath), "Use a fresh isolated preview runtime");
        MpvPreviewPlayer preview;
        Check(preview.Initialize(first.Get(), nullptr), "Actual preview initialization failed");
        MpvPreviewTestAccess::Instrument(preview);
        Check(!preview.IsActive() && !preview.UpdateFrame(first.Get()), "Idle preview rendered");
        preview.SetPreviewOutputSize(640, 360);
        preview.StartPreview(argv[1], 1);
        Until([&] { return preview.UpdateFrame(first.Get()) && preview.GetPreviewBitmap(); }, "Hover produced no decoded bitmap");
        const auto size = preview.GetPreviewBitmap()->GetPixelSize();
        Check(size.width == 640 && size.height == 360, "Hover bitmap dimensions changed");
        const auto initialLoads = MpvPreviewTestAccess::loads;
        for (int i = 0; i < 100; ++i) preview.StartPreview(argv[1], 1);
        Check(MpvPreviewTestAccess::loads == initialLoads, "Unchanged hover restarted decoding");
        const auto hoverRenders = Hold(preview, first.Get(), 500);
        Check(hoverRenders > 1, "Continuous hover stopped after its first frame");
        ComPtr<ID2D1Bitmap> oldBitmap = preview.GetPreviewBitmap();
        preview.BindDeviceContext(second.Get());
        Check(!preview.GetPreviewBitmap(), "Bitmap survived a different D2D device");
        Until([&] { return preview.UpdateFrame(second.Get()) && preview.GetPreviewBitmap(); }, "New device failed to upload hover frame");
        Check(preview.GetPreviewBitmap() != oldBitmap.Get(), "New device reused old bitmap");
        ComPtr<ID2D1Bitmap1> target;
        Check(SUCCEEDED(second->CreateBitmap(D2D1::SizeU(16,16), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)), &target)), "Device target creation failed");
        second->SetTarget(target.Get()); second->BeginDraw();
        second->DrawBitmap(preview.GetPreviewBitmap(), D2D1::RectF(0,0,16,16));
        Check(SUCCEEDED(second->EndDraw()), "Reuploaded bitmap belongs to the wrong device");
        second->SetTarget(nullptr);
        preview.StopPreview();
        Check(!preview.IsActive() && preview.GetCurrentPreviewPath().empty(), "Stop kept hover active");
        preview.BindDeviceContext(nullptr);
        Check(MpvPreviewTestAccess::GraphicsReleased(preview), "Detached preview kept graphics resources");
        unsigned stoppedUpdates = 0;
        const auto stoppedRenders = Hold(preview, second.Get(), 200, &stoppedUpdates);
        Check(stoppedRenders == 0 && stoppedUpdates == 0, "Stopped hover kept rendering");
        preview.StartPreview(argv[1], 2);
        Until([&] { return preview.UpdateFrame(second.Get()) && preview.GetPreviewBitmap(); }, "Stopped preview could not restart");
        preview.SetPreviewOutputSize(800, 450); preview.StartPreview(argv[1], 2);
        Until([&] { return preview.UpdateFrame(second.Get()) && preview.GetPreviewBitmap(); }, "Resized hover produced no frame");
        Check(preview.GetPreviewBitmap()->GetPixelSize().width == 800 && preview.GetPreviewBitmap()->GetPixelSize().height == 450,
            "Same-file preview did not adopt requested dimensions");
        preview.Shutdown();
        Check(MpvPreviewTestAccess::Released(preview) && !preview.IsActive(), "Shutdown retained preview resources");
        Check(!preview.UpdateFrame(second.Get()), "Shutdown preview rendered");
        Check(MpvPreviewTestAccess::Released(preview), "Inactive update reacquired a graphics device after shutdown");
        Check(preview.Initialize(second.Get(), nullptr), "Preview could not reinitialize");
        MpvPreviewTestAccess::Instrument(preview);
        preview.StartPreview(argv[1], 3); preview.StopPreview();
        Check(Hold(preview, second.Get(), 100) == 0, "Immediate cancellation kept rendering");
        preview.StartPreview(argv[1], 1);
        Until([&] { return preview.UpdateFrame(second.Get()) && preview.GetPreviewBitmap(); }, "Cancelled preview could not restart");
        preview.Shutdown();
        Check(MpvPreviewTestAccess::Released(preview) && !FileExists(cachedPath), "Hover decoding persisted a thumbnail or retained resources");
        MpvHubExtractor extractor;
        auto extract = [&](double time) {
            Check(extractor.Begin(argv[1], time), "Actual hub extractor did not start");
            PreviewThumbnailFrame frame;
            Until([&] { extractor.Pump(); return extractor.TakeResult(frame); }, "Hub extractor produced no frame");
            Check(frame.width == 320 && frame.height == 180 && frame.bgraPixels.size() == 320 * 180 * 4, "Hub frame incomplete");
            Check(std::any_of(frame.bgraPixels.begin(), frame.bgraPixels.end(), [](uint8_t pixel) { return pixel != 0; }), "Hub frame empty");
            extractor.Shutdown();
        };
        extractor.SetPrivateMode(true); extract(1);
        Check(!FileExists(cachedPath), "Private hub extraction persisted pixels");
        extractor.SetPrivateMode(false); extract(2);
        PreviewThumbnailFrame stored;
        Check(LoadHubThumbnailFromDiskCache(argv[1], 320, 180, stored, GetExeDir(), false), "Normal hub extraction did not persist");
        Check(PruneOrphanedHubThumbnailDiskCache({argv[1]}) == 0 && FileExists(cachedPath), "Prune removed live decoded hub fixture");
        Check(PruneOrphanedHubThumbnailDiskCache({}) == 1, "Cannot prune normal hub fixture");
        Check(!LoadHubThumbnailFromDiskCache(argv[1], 320, 180, stored, GetExeDir(), false), "Prune left decoded hub pixels readable");
        Check(extractor.Begin(argv[1], 3), "Cancellable hub extraction did not start");
        extractor.Cancel();
        Check(!extractor.IsActive() && !extractor.Pump() && !extractor.TakeResult(stored), "Cancelled hub extraction published pixels");
        extractor.Shutdown();
        Check(!FileExists(cachedPath), "Cancelled hub extraction persisted pixels");
        std::cout << "PASS actual_hover_decode=true unchanged_hover_loads=1 new_device_bitmap_draw=true stop_restart=true"
            << " resized_same_file=true shutdown_reinitialize=true hover_persistence=false actual_hub_decode=true"
            << " private_hub_persistence=false normal_hub_persistence=true cancelled_hub_persistence=false"
            << " stopped_render_calls=" << stoppedRenders << " stopped_bitmap_updates=" << stoppedUpdates
            << " continuous_hover_render_calls=" << hoverRenders << "\n";
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << "\n";
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
}
