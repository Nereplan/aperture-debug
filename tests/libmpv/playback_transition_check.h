// Stateful rendered-frame correctness. Windows remain hidden by default and
// no resource, frame-rate or startup performance measurements are collected.
static void Scenario_TransitionCheck(const std::string& media) {
    const std::string prefix = g_resourceCapture.empty() ? "transition-frame" : g_resourceCapture;
    const char* stages[] = {"initial", "zoom", "unzoom", "rotate", "unrotate",
        "rgba8", "auto-fbo", "software", "hardware", "hdr", "sdr-return", "reopen"};
    for (const char* stage : stages) {
        if (GetFileAttributesA((prefix + "." + stage + ".png").c_str()) != INVALID_FILE_ATTRIBUTES) {
            Report("transition captures are new", false, "preserve existing captures");
            return;
        }
    }
    HWND window = CreateTestWindow(1280, 800);
    auto* player = NewPlayer({
        {"vo", "gpu"}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"},
        {"hwdec", "d3d11va"}, {"audio", "no"}, {"sub", "no"},
        {"force-window", "yes"}, {"keep-open", "yes"}, {"pause", "yes"},
        {"volume", "0"}}, window);
    if (!player) { Report("transition engine initializes", false); DestroyWindow(window); return; }

    auto load = [&](const char* file) {
        // Drain old events first: an earlier file's cached properties must not
        // satisfy readiness for a replacement that has not loaded yet.
        for (int n = 0; n < 1000; ++n) {
            auto* event = WaitEvent(player, 0);
            if (!event || event->event_id == MPV_EVENT_NONE) break;
        }
        const std::string path = media + "/" + file;
        const char* command[] = {"loadfile", path.c_str(), nullptr};
        if (fn_command(player, command) < 0) return false;
        bool loaded = false;
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(25)) {
            auto* event = WaitEvent(player, 0.02);
            if (event && event->event_id == MPV_EVENT_FILE_LOADED) { loaded = true; break; }
        }
        const char* seek[] = {"seek", "2.0", "absolute+exact", nullptr};
        std::string log;
        return loaded && fn_command(player, seek) >= 0 && WaitForCondition(player, [&] {
            return GetStr(player, "seeking") == "no" &&
                std::abs(GetDbl(player, "time-pos") - 2.0) < 0.02;
        }, 10, &log);
    };
    auto capture = [&](const char* stage, bool compatible, const char* expectedDecoder) {
        const auto settle = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - settle < std::chrono::milliseconds(300))
            WaitEvent(player, 0.01);
        const std::string output = prefix + "." + stage + ".png";
        const char* command[] = {"screenshot-to-file", output.c_str(), "window", nullptr};
        bool ok = fn_command(player, command) >= 0;
        const auto passes = GetStr(player, "vo-passes");
        const auto redraw = passes.find("\"redraw\":");
        const bool direct = redraw != std::string::npos && passes.find("direct NV12", redraw) != std::string::npos;
        const bool expectedDirect = compatible && g_frameDirectNv12 == "yes" &&
            g_resourceScaling == "balanced" && g_renderer == "gpu";
        const auto decoder = GetStr(player, "hwdec-current");
        const double position = GetDbl(player, "time-pos");
        ok = ok && decoder == expectedDecoder && GetStr(player, "pause") == "yes" &&
            std::abs(position - 2.0) < 0.02 && direct == expectedDirect;
        Report((std::string("transition ") + stage).c_str(), ok);
        printf("TRANSITION_FRAME {\"stage\":\"%s\",\"headless\":%s,\"position\":%.6f,"
               "\"hwdec\":\"%s\",\"direct_filter\":%s,\"expected_direct\":%s}\n",
               stage, g_headless ? "true" : "false", position, decoder.c_str(),
               direct ? "true" : "false", expectedDirect ? "true" : "false");
        return ok;
    };
    auto property = [&](const char* name, const char* value) {
        const bool ok = fn_set_property_string(player, name, value) >= 0;
        Report((std::string("transition option ") + name + "=" + value).c_str(), ok);
        return ok;
    };
    bool ok = load("h264_aac_720p.mp4") && capture("initial", true, "d3d11va");
    if (ok) ok = property("video-zoom", "0.5") && capture("zoom", false, "d3d11va");
    if (ok) ok = property("video-zoom", "0") && capture("unzoom", true, "d3d11va");
    if (ok) ok = property("video-rotate", "90") && capture("rotate", false, "d3d11va");
    if (ok) ok = property("video-rotate", "0") && capture("unrotate", true, "d3d11va");
    if (ok) ok = property("fbo-format", "rgba8") && capture("rgba8", false, "d3d11va");
    if (ok) ok = property("fbo-format", "auto") && capture("auto-fbo", true, "d3d11va");
    if (ok) ok = property("hwdec", "no") && load("h264_aac_720p.mp4") && capture("software", false, "no");
    if (ok) ok = property("hwdec", "d3d11va") && load("h264_aac_720p.mp4") && capture("hardware", true, "d3d11va");
    if (ok) ok = load("hevc_hdr10.mp4") && capture("hdr", false, "d3d11va");
    if (ok) ok = load("h264_aac_720p.mp4") && capture("sdr-return", true, "d3d11va");
    if (ok) {
        std::string log;
        ok = fn_command_string(player, "stop") >= 0 && WaitForCondition(player, [&] {
            return GetStr(player, "idle-active") == "yes";
        }, 10, &log);
        Report("transition stop becomes idle", ok);
    }
    if (ok) ok = load("h264_aac_720p.mp4") && capture("reopen", true, "d3d11va");
    Report("transition sequence completes", ok);
    if (ok) Report("transition effective options", ReportPlaybackRenderOptions(player));
    DestroyPlayer(player);
    DestroyWindow(window);
}
