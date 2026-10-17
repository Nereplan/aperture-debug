// Included after the functional harness helpers. Each invocation measures one
// fresh engine. PLAYBACK_RESTART is engine readiness, not compositor presentation.
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
    auto* player = NewPlayer({{"vo", "gpu"}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"},
        {"hwdec", "d3d11va"}, {"hwdec-extra-frames", "0"}, {"video-latency-hacks", "yes"},
        {"video-sync", g_videoSync}, {"vd-lavc-threads", g_decoderThreads}, {"framedrop", "vo"}, {"video-timing-offset", "0"},
        {"vd-lavc-dr", "yes"}, {"demuxer-max-bytes", "8MiB"}, {"demuxer-max-back-bytes", "0"},
        {"audio", g_resourceAudio ? "auto" : "no"}, {"volume", "0"},
        {"sub", "no"}, {"loop-file", "inf"}, {"force-window", "yes"}}, window);
    if (!player) { Report("resource engine initialization", false); DestroyWindow(window); return; }
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
        const double cpuStart = ProcessCpuSeconds();
        const auto sampleStart = std::chrono::steady_clock::now();
        const double position = GetDbl(player, "time-pos");
        const auto initialDrops = GetInt(player, "frame-drop-count");
        while (std::chrono::steady_clock::now() - sampleStart < std::chrono::seconds(3)) WaitEvent(player, 0.01);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - sampleStart).count();
        const double cpu = (ProcessCpuSeconds() - cpuStart) / elapsed * 100.0;
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
        printf("RESOURCE {\"renderer\":\"%s\",\"file\":\"%s\",\"sync\":\"%s\",\"decoder_threads\":\"%s\",\"audio_enabled\":%s,\"init_ms\":%.3f,\"restart_ms\":%.3f,"
            "\"cpu_one_core_percent\":%.3f,\"working_set\":%llu,\"private_bytes\":%llu,"
            "\"gpu_local_bytes\":%llu,\"gpu_shared_bytes\":%llu,\"dropped_frames\":%lld,\"steady_drops\":%lld,\"hwdec\":\"%s\"}\n",
            g_renderer.c_str(), file.c_str(), g_videoSync.c_str(), g_decoderThreads.c_str(),
            g_resourceAudio ? "true" : "false", initializeMs, readyMs, cpu,
            static_cast<unsigned long long>(memory.WorkingSetSize), static_cast<unsigned long long>(memory.PrivateUsage),
            static_cast<unsigned long long>(localBytes), static_cast<unsigned long long>(sharedBytes),
            static_cast<long long>(GetInt(player, "frame-drop-count")),
            static_cast<long long>(GetInt(player, "frame-drop-count") - initialDrops), hwdec.c_str());
    }
    DestroyPlayer(player); DestroyWindow(window);
}
