#include "MpvPlayer.h"
#include <iostream>

using namespace Aperture;

namespace Aperture {
// Instrument the real software renderer without adding production counters.
struct MpvPreviewTestAccess {
    inline static decltype(&mpv_render_context_render) render = nullptr;
    inline static decltype(&mpv_command) command = nullptr;
    inline static unsigned renders = 0, seeks = 0;
    static int Render(mpv_render_context* context, mpv_render_param* params) {
        ++renders;
        return render(context, params);
    }
    static int Command(mpv_handle* handle, const char** args) {
        if (args && args[0] && std::strcmp(args[0], "seek") == 0) ++seeks;
        return command(handle, args);
    }
    static void Instrument(MpvPreviewPlayer& player) {
        render = player.fn_mpv_render_context_render;
        command = player.fn_mpv_command;
        player.fn_mpv_render_context_render = &Render;
        player.fn_mpv_command = &Command;
        renders = seeks = 0;
    }
    static bool Paused(MpvPreviewPlayer& player) {
        int paused = 0;
        return player.fn_mpv_get_property(player.mpv, "pause", MPV_FORMAT_FLAG, &paused) >= 0 && paused;
    }
    static double Position(MpvPreviewPlayer& player) {
        double position = -1;
        player.fn_mpv_get_property(player.mpv, "time-pos", MPV_FORMAT_DOUBLE, &position);
        return position;
    }
    static bool CapturePending(const MpvPreviewPlayer& player) { return player.seekbarCapturePending; }
    static bool SeekPending(const MpvPreviewPlayer& player) { return player.hasPendingSeek; }
    static void HoldSaveQueue(MpvPreviewPlayer& player) { player.saveWorker = std::thread([] {}); }
    static void QueueSave(MpvPreviewPlayer& player, PreviewThumbnailFrame&& frame) { player.QueueDiskSave(std::move(frame)); }
    static const uint8_t* QueuedPixels(const MpvPreviewPlayer& player) { return player.saveQueue.back().bgraPixels.data(); }
    static size_t QueuedCount(const MpvPreviewPlayer& player) { return player.saveQueue.size(); }
    static double OldestSaveTime(const MpvPreviewPlayer& player) { return player.saveQueue.front().timestampSec; }
    static void FlushSaveQueue(MpvPreviewPlayer& player) {
        player.saveWorker.join();
        player.saveWorker = std::thread(&MpvPreviewPlayer::SaveWorkerThread, &player);
        player.StopSaveWorker();
    }
    static void ExpireSeek(MpvPreviewPlayer& player) { player.lastSeekTime -= std::chrono::seconds(6); }
    inline static bool syntheticRestart = false, syntheticSeeking = false;
    inline static double syntheticPosition = 0;
    inline static unsigned syntheticRenders = 0;
    static mpv_event* Event(mpv_handle*, double) {
        static mpv_event event{};
        event.event_id = syntheticRestart ? MPV_EVENT_PLAYBACK_RESTART : MPV_EVENT_NONE;
        syntheticRestart = false;
        return &event;
    }
    static int Property(mpv_handle*, const char* name, mpv_format, void* output) {
        if (std::strcmp(name, "seeking") == 0) *static_cast<int*>(output) = syntheticSeeking;
        else if (std::strcmp(name, "time-pos") == 0) *static_cast<double*>(output) = syntheticPosition;
        else if (std::strcmp(name, "container-fps") == 0) *static_cast<double*>(output) = 10;
        else return MPV_ERROR_PROPERTY_NOT_FOUND;
        return 0;
    }
    static int SetProperty(mpv_handle*, const char*, const char*) { return 0; }
    static int SyntheticCommand(mpv_handle*, const char**) { return 0; }
    static uint64_t Update(mpv_render_context*) { return MPV_RENDER_UPDATE_FRAME; }
    static int SyntheticRender(mpv_render_context*, mpv_render_param* params) {
        ++syntheticRenders;
        for (auto* param = params; param->type != MPV_RENDER_PARAM_INVALID; ++param) {
            if (param->type == MPV_RENDER_PARAM_SW_POINTER)
                std::memset(param->data, static_cast<int>(syntheticPosition), 320 * 180 * 4);
        }
        return 0;
    }
    static void MakeSynthetic(MpvPreviewPlayer& player) {
        player.mpv = reinterpret_cast<mpv_handle*>(1);
        player.renderCtx = reinterpret_cast<mpv_render_context*>(1);
        player.fn_mpv_wait_event = &Event;
        player.fn_mpv_get_property = &Property;
        player.fn_mpv_set_property_string = &SetProperty;
        player.fn_mpv_command = &SyntheticCommand;
        player.fn_mpv_render_context_update = &Update;
        player.fn_mpv_render_context_render = &SyntheticRender;
        player.pixelBuffer.assign(320 * 180 * 4, 0);
        player.SetPrivateMode(true);
        syntheticRestart = syntheticSeeking = false;
        syntheticPosition = 0;
        syntheticRenders = 0;
    }
};
}

static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Predicate> static void Until(Predicate predicate, const char* message) {
    const auto deadline = GetTickCount64() + 10000;
    do { if (predicate()) return; Sleep(10); } while (GetTickCount64() < deadline);
    throw std::runtime_error(message);
}
static unsigned Hold(MpvPreviewPlayer& preview, ID2D1DeviceContext* context, DWORD durationMs, unsigned* bitmapUpdates = nullptr) {
    const auto before = MpvPreviewTestAccess::renders;
    const auto deadline = GetTickCount64() + durationMs;
    do { if (preview.UpdateFrame(context) && bitmapUpdates) ++*bitmapUpdates; Sleep(10); } while (GetTickCount64() < deadline);
    return MpvPreviewTestAccess::renders - before;
}
static void CheckDiskSaveOwnership() {
    const auto file = L"C:\\Aperture-preview-save-" + std::to_wstring(GetCurrentProcessId()) + L".mp4";
    auto makeFrame = [&](double time, uint8_t value) {
        PreviewThumbnailFrame frame;
        frame.filePath = file; frame.timestampSec = time; frame.width = 320; frame.height = 180;
        frame.bgraPixels.assign(320 * 180 * 4, value);
        return frame;
    };
    MpvPreviewPlayer preview;
    MpvPreviewTestAccess::HoldSaveQueue(preview); // A joinable sentinel makes queue inspection deterministic.
    auto frame = makeFrame(12, 31);
    preview.GetMemoryCache().Put(frame);
    const auto* firstBuffer = frame.bgraPixels.data();
    MpvPreviewTestAccess::QueueSave(preview, std::move(frame));
    Check(MpvPreviewTestAccess::QueuedPixels(preview) == firstBuffer, "Disk queue copied the captured pixel allocation");
    PreviewThumbnailFrame cached;
    Check(preview.GetMemoryCache().TryGet(file, 12, cached) && cached.bgraPixels == std::vector<uint8_t>(320 * 180 * 4, 31),
        "Transferring the disk frame damaged the memory cache");

    frame = makeFrame(12, 79);
    preview.GetMemoryCache().Put(frame);
    const auto expectedPixels = frame.bgraPixels;
    const auto* replacementBuffer = frame.bgraPixels.data();
    MpvPreviewTestAccess::QueueSave(preview, std::move(frame));
    Check(MpvPreviewTestAccess::QueuedCount(preview) == 1 && MpvPreviewTestAccess::QueuedPixels(preview) == replacementBuffer,
        "Coalesced disk save copied pixels or added a duplicate");
    preview.SetPrivateMode(true);
    frame = makeFrame(20, 100);
    const auto* privateBuffer = frame.bgraPixels.data();
    MpvPreviewTestAccess::QueueSave(preview, std::move(frame));
    Check(frame.bgraPixels.data() == privateBuffer && MpvPreviewTestAccess::QueuedCount(preview) == 1,
        "Private request consumed or queued captured pixels");
    preview.SetPrivateMode(false);
    MpvPreviewTestAccess::FlushSaveQueue(preview);
    std::string stored, expected;
    Check(ThumbnailCodec::Encode(expectedPixels, 320, 180, 12, expected), "Expected disk-frame encoding failed");
    Check(ReadCacheFile(CacheClass::Thumbnails, MakeThumbnailCacheKey(file, 12), stored).is_ok() && stored == expected,
        "Moved/coalesced disk save changed stored bytes");
    Check(preview.GetMemoryCache().TryGet(file, 12, cached) && cached.bgraPixels == expectedPixels,
        "Background persistence consumed the memory-cache frame");
    PreviewThumbnailFrame persisted;
    Check(LoadThumbnailFromDiskCache(file, 12, persisted) && persisted.width == 320 && persisted.height == 180 &&
        persisted.timestampSec == 12 && persisted.bgraPixels.size() == expectedPixels.size(), "Saved frame did not round-trip");

    MpvPreviewPlayer bounded;
    MpvPreviewTestAccess::HoldSaveQueue(bounded);
    for (int time = 0; time < 12; ++time) MpvPreviewTestAccess::QueueSave(bounded, makeFrame(time, static_cast<uint8_t>(time)));
    Check(MpvPreviewTestAccess::QueuedCount(bounded) == 8 && MpvPreviewTestAccess::OldestSaveTime(bounded) == 4,
        "Moving saves bypassed the bounded newest-frame queue");
}
static void CheckSyntheticCapture(ID2D1DeviceContext* context) {
    MpvPreviewPlayer preview;
    MpvPreviewTestAccess::MakeSynthetic(preview);
    const auto file = L"C:\\Aperture-preview-synthetic-" + std::to_wstring(GetCurrentProcessId()) + L".mp4";
    preview.RequestSeekbarThumbnail(file, 12, 45, 9, context);
    preview.UpdateFrame(context);
    Check(!preview.HasSeekbarThumbnail(), "Old redraw was accepted before seek completion");
    MpvPreviewTestAccess::syntheticRestart = true;
    MpvPreviewTestAccess::syntheticSeeking = true;
    preview.UpdateFrame(context);
    Check(!preview.HasSeekbarThumbnail(), "Incomplete seek was cached");
    MpvPreviewTestAccess::syntheticSeeking = false;
    preview.UpdateFrame(context);
    Check(!preview.HasSeekbarThumbnail(), "Restart at old position was cached");
    MpvPreviewTestAccess::syntheticPosition = 12;
    preview.UpdateFrame(context);
    Check(preview.HasSeekbarThumbnail(), "Completed synthetic target was not captured");
    const auto renders = MpvPreviewTestAccess::syntheticRenders;
    for (int i = 0; i < 100; ++i) preview.UpdateFrame(context);
    Check(MpvPreviewTestAccess::syntheticRenders == renders, "Synthetic redraws repeated the completed capture");
    PreviewThumbnailFrame captured;
    Check(preview.GetMemoryCache().TryGet(file, 12, captured) && captured.bgraPixels.front() == 12,
        "Synthetic cache contains pixels from before the target seek");
    preview.ResetSeekbarPreview();
    Check(!MpvPreviewTestAccess::CapturePending(preview), "Reset kept synthetic capture armed");
    preview.RequestSeekbarThumbnail(file, 33, 45, 9, context);
    MpvPreviewTestAccess::ExpireSeek(preview);
    preview.UpdateFrame(context);
    Check(!MpvPreviewTestAccess::CapturePending(preview) && !preview.IsSeekbarThumbnailLoading(),
        "Unfinished extraction did not expire");
    Check(!preview.RequestSeekbarThumbnail(file, 33, 45, 9, context),
        "Repeated expired request restarted extraction");
    const auto expiredRenders = MpvPreviewTestAccess::syntheticRenders;
    for (int i = 0; i < 100; ++i) preview.UpdateFrame(context);
    Check(MpvPreviewTestAccess::syntheticRenders == expiredRenders, "Expired extraction kept rendering");
    preview.ResetSeekbarPreview();
    Check(preview.RequestSeekbarThumbnail(file, 33, 45, 9, context), "Reset did not allow a new attempt");
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
        CheckDiskSaveOwnership();
        CheckSyntheticCapture(first.Get());
        MpvPreviewPlayer preview;
        Check(preview.Initialize(first.Get(), nullptr), "Actual preview profile initialization failed");
        MpvPreviewTestAccess::Instrument(preview);
        preview.SetPreviewOutputSize(640, 360);
        Check(!preview.IsSeekbarActive() && !preview.HasSeekbarThumbnail(), "Idle preview allocated active seekbar state");
        preview.StartPreview(argv[1], 1);
        Until([&] { preview.UpdateFrame(first.Get()); return preview.GetPreviewBitmap() != nullptr; }, "Hover pipeline produced no decoded bitmap");
        const auto size = preview.GetPreviewBitmap()->GetPixelSize();
        Check(size.width == 640 && size.height == 360, "Hover bitmap dimensions changed");
        const auto hoverRenders = Hold(preview, first.Get(), 500);
        Check(hoverRenders > 1, "Continuous hover stopped after its first frame");
        preview.StopPreview();
        preview.RequestSeekbarThumbnail(argv[1], 2, 45, 7, first.Get());
        Until([&] { preview.UpdateFrame(first.Get()); return preview.HasSeekbarThumbnail(); }, "Dormant seekbar extraction produced no thumbnail");
        Check(MpvPreviewTestAccess::Paused(preview), "Completed seekbar capture kept playing");
        PreviewThumbnailFrame original;
        Check(preview.GetMemoryCache().TryGet(argv[1], 2, original), "Captured frame is absent from memory cache");
        unsigned holdUpdates = 0;
        const auto holdRenders = Hold(preview, first.Get(), 1000, &holdUpdates);
        Check(holdRenders == 0, "Stationary seekbar thumbnail kept rendering");
        PreviewThumbnailFrame held;
        Check(preview.GetMemoryCache().TryGet(argv[1], 2, held) && held.bgraPixels == original.bgraPixels,
            "Stationary thumbnail changed after capture");
        // A paused seek must finish at its requested position, even though the
        // renderer still has an old frame available when the command is sent.
        preview.RequestSeekbarThumbnail(argv[1], 20, 45, 7, first.Get());
        const auto coalescedSeeks = MpvPreviewTestAccess::seeks;
        for (int i = 0; i < 100; ++i) preview.RequestSeekbarThumbnail(argv[1], 20.1, 45, 7, first.Get());
        Check(MpvPreviewTestAccess::seeks == coalescedSeeks, "Repeated same-bucket requests restarted their seek");
        Until([&] { preview.UpdateFrame(first.Get()); return preview.HasSeekbarThumbnail(); }, "Paused forward seek produced no target frame");
        Check(std::abs(MpvPreviewTestAccess::Position(preview) - 20) < 0.15, "Forward seek captured the previous frame");
        PreviewThumbnailFrame forward;
        Check(preview.GetMemoryCache().TryGet(argv[1], 20, forward) && forward.bgraPixels != original.bgraPixels,
            "Forward target reused the old pixels");
        preview.RequestSeekbarThumbnail(argv[1], 5, 45, 7, first.Get());
        Until([&] { preview.UpdateFrame(first.Get()); return preview.HasSeekbarThumbnail(); }, "Paused backward seek produced no target frame");
        Check(std::abs(MpvPreviewTestAccess::Position(preview) - 5) < 0.15, "Backward seek captured the previous frame");
        // The next request is debounced. A cached request supersedes it before
        // it executes and must also disarm any in-flight extraction.
        preview.RequestSeekbarThumbnail(argv[1], 30, 45, 7, first.Get());
        preview.RequestSeekbarThumbnail(argv[1], 31, 45, 7, first.Get());
        preview.RequestSeekbarThumbnail(argv[1], 2, 45, 7, first.Get());
        Check(!MpvPreviewTestAccess::SeekPending(preview) && !MpvPreviewTestAccess::CapturePending(preview),
            "Cached request left an extraction pending");
        const auto cancelledSeeks = MpvPreviewTestAccess::seeks;
        Check(Hold(preview, first.Get(), 200) == 0 && MpvPreviewTestAccess::seeks == cancelledSeeks,
            "A cached request executed a superseded seek");
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
        std::cout << "PASS actual_hover_decode=true dormant_seekbar_decode=true new_device_cache_upload=true actual_hub_decode=true"
            << " stationary_hold_render_calls=" << holdRenders << " continuous_hover_render_calls=" << hoverRenders
            << " stationary_hold_bitmap_updates=" << holdUpdates
            << " forward_backward_target_verified=true repeated_request_count=100 cached_cancellation=true expired_request_bounded=true"
            << " disk_queue_pixel_ownership_transferred=true coalesced_stored_bytes_exact=true memory_cache_lifetime_preserved=true bounded_save_queue=true\n";
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << "\n";
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
}
