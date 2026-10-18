// Hidden fixed-frame correctness checks. No CPU, memory, startup or playback
// throughput measurement is emitted from an occluded window.
static void Scenario_FrameCheck(const std::string& media, const std::string& file) {
    if (g_resourceCapture.empty() || file.empty()) {
        Report("frame check arguments", false, "requires --resource-file and --capture-window");
        return;
    }
    if (GetFileAttributesA(g_resourceCapture.c_str()) != INVALID_FILE_ATTRIBUTES) {
        Report("frame check output is new", false, "preserve the existing capture");
        return;
    }
    HWND window = CreateTestWindow(1280, 800);
    std::vector<std::pair<std::string, std::string>> options{
        {"vo", "gpu"}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"},
        {"hwdec", g_resourceHwdec}, {"audio", "no"}, {"sub", "no"},
        {"force-window", "yes"}, {"keep-open", "yes"}, {"volume", "0"}};
    if (!g_resourceFboFormat.empty()) options.emplace_back("fbo-format", g_resourceFboFormat);
    if (!g_frameDither.empty()) options.emplace_back("dither", g_frameDither);
    if (g_frameRotation) options.emplace_back("video-rotate", std::to_string(g_frameRotation));
    if (g_frameZoom) options.emplace_back("video-zoom", std::to_string(g_frameZoom));
    auto* player = NewPlayer(options, window);
    if (!player) { Report("frame check engine initializes", false); DestroyWindow(window); return; }
    const std::string path = media + "/" + file;
    const char* load[] = {"loadfile", path.c_str(), nullptr};
    std::string log;
    bool ready = fn_command(player, load) >= 0 && WaitForCondition(player, [&] {
        return GetDbl(player, "time-pos") > 0.2 && GetInt(player, "video-out-params/w") > 0;
    }, 25, &log);
    Report("frame check playback starts", ready, log);
    if (ready) {
        fn_set_property_string(player, "pause", "yes");
        const char* seek[] = {"seek", "2.0", "absolute+exact", nullptr};
        const bool settled = fn_command(player, seek) >= 0 && WaitForCondition(player, [&] {
            return GetStr(player, "seeking") == "no" &&
                std::abs(GetDbl(player, "time-pos") - 2.0) < 0.02;
        }, 10, &log);
        Report("frame check exact paused position", settled, log);
        const auto pauseStart = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - pauseStart < std::chrono::milliseconds(300)) WaitEvent(player, 0.01);
        const char* shot[] = {"screenshot-to-file", g_resourceCapture.c_str(), "window", nullptr};
        Report("frame check rendered capture", settled && fn_command(player, shot) >= 0);
        const auto drainStart = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - drainStart < std::chrono::milliseconds(100)) WaitEvent(player, 0.01);
        const auto hwdec = GetStr(player, "hwdec-current");
        Report("frame check requested decoder", hwdec == g_resourceHwdec);
        Report("frame check effective options", ReportPlaybackRenderOptions(player));
        const auto passes = GetStr(player, "vo-passes");
        Report("frame check render pass data", !passes.empty() && passes[0] == '{');
        if (!passes.empty()) printf("RENDER_PASSES %s\n", passes.c_str());
        // Use the last explicit redraw's generated pass descriptions as proof;
        // merely requesting the option does not establish which path rendered.
        const auto redraw = passes.find("\"redraw\":");
        const bool direct = redraw != std::string::npos &&
            passes.find("direct NV12", redraw) != std::string::npos;
        printf("FRAME_CHECK {\"headless\":%s,\"position\":%.6f,\"hwdec\":\"%s\","
               "\"selected_fbo\":\"%s\",\"direct_filter\":\"%s\"}\n",
               g_headless ? "true" : "false", GetDbl(player, "time-pos"), hwdec.c_str(),
               g_observedFboFormat.c_str(),
               direct ? "yes" : "no");
    }
    DestroyPlayer(player);
    DestroyWindow(window);
}
