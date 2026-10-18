// Included after the functional harness helpers. Each invocation measures one
// fresh engine. PLAYBACK_RESTART is engine readiness, not compositor presentation.
#include "playback_memory_layout.h"
#include "playback_render_options.h"
#include "../../src/PlaybackScaling.h"
static double ProcessCpuSeconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    ULARGE_INTEGER k{}, u{}; k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) / 10000000.0;
}

static void Scenario_Resources(const std::string& media, const std::string& file) {
    const auto start = std::chrono::steady_clock::now();
    HWND window = CreateTestWindow(1280, 800);
    std::vector<std::pair<std::string, std::string>> options{{"vo", "gpu"}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"},
        {"hwdec", "d3d11va"}, {"hwdec-extra-frames", "0"}, {"video-latency-hacks", g_resourceLatencyHacks},
        {"video-sync", g_videoSync}, {"vd-lavc-threads", g_decoderThreads}, {"framedrop", "vo"}, {"video-timing-offset", "0"},
        {"vd-lavc-dr", "yes"}, {"demuxer-max-bytes", "8MiB"}, {"demuxer-max-back-bytes", "0"},
        {"audio", g_resourceAudio ? "auto" : "no"}, {"volume", "0"},
        {"sub", "no"}, {"loop-file", "inf"}, {"force-window", "yes"}};
    if (!g_resourceZeroCopy.empty()) options.emplace_back("d3d11va-zero-copy", g_resourceZeroCopy);
    if (!g_resourceSwapchain.empty()) options.emplace_back("swapchain-depth", g_resourceSwapchain);
    if (!g_resourceSyncInterval.empty()) options.emplace_back("d3d11-sync-interval", g_resourceSyncInterval);
    if (g_resourceScaling == "linear") {
        for (const auto& option : Aperture::PlaybackScaling::kLinear) options.emplace_back(option.name, option.value);
    }
    if (g_resourceScaling == "balanced") {
        for (const auto& option : Aperture::PlaybackScaling::kBalanced) options.emplace_back(option.name, option.value);
    }
    auto* player = NewPlayer(options, window);
    if (!player) { Report("resource engine initialization", false); DestroyWindow(window); return; }
    if (g_resourceOptionsOnly) {
        Report("effective renderer options available", ReportPlaybackRenderOptions(player));
        DestroyPlayer(player); DestroyWindow(window); return;
    }
    const double initializeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const auto path = media + "/" + (file.empty() ? "h264_aac_720p.mp4" : file);
    const char* command[] = {"loadfile", path.c_str(), nullptr};
    bool restarted = false;
    fn_command(player, command);
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(25)) {
        MSG message;
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessage(&message);
        }
        const auto* event = fn_wait_event(player, 0.005);
        if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) { restarted = true; break; }
        if (event->event_id == MPV_EVENT_END_FILE) break;
    }
    const auto ready = std::chrono::steady_clock::now();
    const double readyMs = std::chrono::duration<double, std::milli>(ready - start).count();
    Report("resource playback restart", restarted);
    if (restarted) {
        while (std::chrono::steady_clock::now() - ready < std::chrono::seconds(5)) WaitEvent(player, 0.01);
        double cleanupMs = 0;
        uint64_t cleanupBefore = 0, cleanupAfter = 0;
        if (g_resourceHeapCleanup) {
            PROCESS_MEMORY_COUNTERS_EX before{}; before.cb = sizeof(before);
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof(before));
            cleanupBefore = before.PrivateUsage;
            HEAP_OPTIMIZE_RESOURCES_INFORMATION cleanup{HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION, 0};
            const auto cleanupStart = std::chrono::steady_clock::now();
            const bool cleaned = HeapSetInformation(nullptr, HeapOptimizeResources, &cleanup, sizeof(cleanup)) != FALSE;
            cleanupMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cleanupStart).count();
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof(before));
            cleanupAfter = before.PrivateUsage;
            Report("resource free heap cache cleanup", cleaned);
        }
        ULONG64 cyclesStart = 0, cyclesEnd = 0;
        const bool cyclesStarted = QueryProcessCycleTime(GetCurrentProcess(), &cyclesStart) != FALSE;
        const double cpuStart = ProcessCpuSeconds();
        const auto sampleStart = std::chrono::steady_clock::now();
        const double position = GetDbl(player, "time-pos");
        const auto initialDrops = GetInt(player, "decoder-frame-drop-count");
        const auto initialVoDrops = GetInt(player, "frame-drop-count");
        while (std::chrono::steady_clock::now() - sampleStart < std::chrono::seconds(g_resourceSampleSeconds)) WaitEvent(player, 0.01);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - sampleStart).count();
        const double cpuEnd = ProcessCpuSeconds();
        const bool cyclesEnded = QueryProcessCycleTime(GetCurrentProcess(), &cyclesEnd) != FALSE;
        const auto finalDrops = GetInt(player, "decoder-frame-drop-count");
        const auto finalVoDrops = GetInt(player, "frame-drop-count");
        const double cpu = (cpuEnd - cpuStart) / elapsed * 100.0;
        Report("resource CPU counter interval", cpuStart >= 0 && cpuEnd >= cpuStart && elapsed > 0);
        Report("resource process cycle counter interval", cyclesStarted && cyclesEnded && cyclesEnd >= cyclesStart);
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
        const bool measuredMemory = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
        uint64_t localBytes = 0, sharedBytes = 0;
        bool measuredGpu = false;
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
            IDXGIAdapter1* adapter = nullptr;
            for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i) {
                IDXGIAdapter3* adapter3 = nullptr;
                if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
                    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
                    if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
                        SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared))) {
                        measuredGpu = true; localBytes += local.CurrentUsage; sharedBytes += shared.CurrentUsage;
                    }
                    adapter3->Release();
                }
                adapter->Release();
            }
            factory->Release();
        }
        Report("resource process counters", measuredMemory && measuredGpu);
        const double finalPosition = GetDbl(player, "time-pos");
        Report("resource playback advances", position >= 0 && finalPosition >= 0 && position != finalPosition);
        const auto hwdec = GetStr(player, "hwdec-current");
        Report("resource direct hardware decoder", hwdec == "d3d11va");
        Report("resource frame-drop counters available", initialDrops >= 0 && finalDrops >= 0 && initialVoDrops >= 0 && finalVoDrops >= 0);
        Report("resource no steady decoding or presentation drops", finalDrops == initialDrops && finalVoDrops == initialVoDrops);
        printf("RESOURCE {\"renderer\":\"%s\",\"file\":\"%s\",\"sync\":\"%s\",\"decoder_threads\":\"%s\",\"audio_enabled\":%s,\"init_ms\":%.3f,\"restart_ms\":%.3f,"
            "\"cpu_one_core_percent\":%.3f,\"working_set\":%llu,\"private_bytes\":%llu,"
            "\"gpu_local_bytes\":%llu,\"gpu_shared_bytes\":%llu,\"dropped_frames\":%lld,\"steady_drops\":%lld,\"hwdec\":\"%s\","
            "\"cpu_start_seconds\":%.8f,\"cpu_end_seconds\":%.8f,\"sample_elapsed_seconds\":%.8f,"
            "\"process_cycles_start\":%llu,\"process_cycles_end\":%llu,"
            "\"position_start\":%.8f,\"position_end\":%.8f,\"vo_steady_drops\":%lld,\"zero_cpu_interval\":%s,"
            "\"zero_copy\":\"%s\",\"swapchain_depth\":\"%s\",\"latency_hacks\":\"%s\",\"hwdec_threads\":\"%s\","
            "\"drop_property\":\"decoder-frame-drop-count\",\"vo_drop_property\":\"frame-drop-count\","
            "\"heap_cleanup\":%s,\"heap_cleanup_ms\":%.3f,\"heap_before_private\":%llu,\"heap_after_private\":%llu}\n",
            g_renderer.c_str(), file.c_str(), g_videoSync.c_str(), g_decoderThreads.c_str(),
            g_resourceAudio ? "true" : "false", initializeMs, readyMs, cpu,
            static_cast<unsigned long long>(memory.WorkingSetSize), static_cast<unsigned long long>(memory.PrivateUsage),
            static_cast<unsigned long long>(localBytes), static_cast<unsigned long long>(sharedBytes),
            static_cast<long long>(finalDrops),
            static_cast<long long>(finalDrops - initialDrops), hwdec.c_str(),
            cpuStart, cpuEnd, elapsed,
            static_cast<unsigned long long>(cyclesStart), static_cast<unsigned long long>(cyclesEnd), position, finalPosition,
            static_cast<long long>(finalVoDrops - initialVoDrops), cpuEnd == cpuStart ? "true" : "false",
            g_resourceZeroCopy.empty() ? "default" : g_resourceZeroCopy.c_str(),
            g_resourceSwapchain.empty() ? "default" : g_resourceSwapchain.c_str(), g_resourceLatencyHacks.c_str(),
            g_resourceHwdecThreads.empty() ? "default" : g_resourceHwdecThreads.c_str(), g_resourceHeapCleanup ? "true" : "false", cleanupMs,
            static_cast<unsigned long long>(cleanupBefore), static_cast<unsigned long long>(cleanupAfter));
        ReportPlaybackRenderOptions(player);
        if (g_resourceMemoryLayout) ReportPlaybackMemoryLayout();
        if (!g_resourceCapture.empty()) {
            fn_set_property_string(player, "pause", "yes");
            const char* seek[] = {"seek", "2.0", "absolute+exact", nullptr};
            fn_command(player, seek);
            const bool settled = WaitForCondition(player, [&] { return std::abs(GetDbl(player, "time-pos") - 2.0) < 0.02; }, 5.0);
            const auto captureReady = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - captureReady < std::chrono::milliseconds(300)) WaitEvent(player, 0.01);
            const char* capture[] = {"screenshot-to-file", g_resourceCapture.c_str(), "window", nullptr};
            Report("resource fixed-frame rendered window capture", settled && fn_command(player, capture) == 0);
        }
    }
    DestroyPlayer(player); DestroyWindow(window);
}
