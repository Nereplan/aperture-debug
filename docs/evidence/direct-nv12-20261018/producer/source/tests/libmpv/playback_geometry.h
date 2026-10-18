// Exercise swap-chain resource lifetime through real resize/reconfigure calls.
// Captures belong to the owned test run directory, never the media fixtures.
static bool GeometryPngSize(const std::string& path, int width, int height) {
    unsigned char bytes[24]{};
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    const size_t count = fread(bytes, 1, sizeof(bytes), file);
    fclose(file);
    const unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    auto be32 = [](const unsigned char* p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
            (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    };
    return count == sizeof(bytes) && !memcmp(bytes, signature, sizeof(signature)) &&
        !memcmp(bytes + 12, "IHDR", 4) && be32(bytes + 16) == uint32_t(width) &&
        be32(bytes + 20) == uint32_t(height);
}

static void Scenario_Geometry(const std::string& media) {
    HWND window = CreateTestWindow(640, 360);
    auto* player = NewPlayer({{"vo", "gpu"}, {"gpu-api", "d3d11"},
        {"gpu-context", "d3d11"}, {"hwdec", "d3d11va"}, {"force-window", "yes"},
        {"keep-open", "yes"}, {"loop-file", "inf"}, {"volume", "0"}}, window);
    if (!player) { Report("geometry player initialization", false); DestroyWindow(window); return; }
    std::string log;
    const auto path = media + "/h264_aac_720p.mp4";
    const char* load[] = {"loadfile", path.c_str(), nullptr};
    const bool started = fn_command(player, load) >= 0 && WaitForCondition(player,
        [&] { return GetDbl(player, "time-pos") > 0.2; }, 20, &log);
    Report("geometry playback starts", started, log);
    if (!started) { DestroyPlayer(player); DestroyWindow(window); return; }

    fn_set_property_string(player, "pause", "yes");
    const char* seek[] = {"seek", "3", "absolute+exact", nullptr};
    const bool paused = fn_command(player, seek) >= 0 && WaitForCondition(player, [&] {
        return GetStr(player, "pause") == "yes" && GetStr(player, "seeking") == "no" &&
            fabs(GetDbl(player, "time-pos") - 3.0) < 0.1;
    }, 10, &log);
    Report("geometry paused exact seek settles", paused, log);

    unsigned capture = 0;
    auto resizeAndCapture = [&](int width, int height, const char* phase) {
        RECT outer{0, 0, width, height};
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        const DWORD extended = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        bool ok = AdjustWindowRectEx(&outer, style, FALSE, extended) &&
            SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left,
                outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        ok = ok && WaitForCondition(player, [&] {
            return GetInt(player, "osd-width") == width && GetInt(player, "osd-height") == height;
        }, 10, &log);
        const auto file = "geometry-" + std::to_string(capture++) + ".png";
        DeleteFileA(file.c_str());
        const char* shot[] = {"screenshot-to-file", file.c_str(), "window", nullptr};
        ok = ok && fn_command(player, shot) >= 0 && GeometryPngSize(file, width, height);
        Report((std::string("geometry ") + phase + " " + std::to_string(width) + "x" +
            std::to_string(height)).c_str(), ok, log);
    };
    resizeAndCapture(640, 360, "paused");
    resizeAndCapture(960, 540, "paused");
    resizeAndCapture(384, 512, "portrait window");
    resizeAndCapture(640, 360, "restored");
    Report("geometry rotate accepts", fn_set_property_string(player, "video-rotate", "90") >= 0);
    resizeAndCapture(480, 640, "rotated");
    Report("geometry unrotate accepts", fn_set_property_string(player, "video-rotate", "0") >= 0);
    fn_set_property_string(player, "pause", "no");
    const double position = GetDbl(player, "time-pos");
    Report("geometry resume advances", WaitForCondition(player, [&] {
        return GetDbl(player, "time-pos") > position + 0.3;
    }, 5, &log), log);
    resizeAndCapture(800, 450, "playing");

    const auto next = media + "/hevc_hdr10.mp4";
    const char* reload[] = {"loadfile", next.c_str(), nullptr};
    const bool reloaded = fn_command(player, reload) >= 0 && WaitForCondition(player, [&] {
        return GetStr(player, "video-format").find("hevc") != std::string::npos &&
            GetDbl(player, "time-pos") > 0.2;
    }, 20, &log);
    Report("geometry reconfigure H264 to HDR HEVC", reloaded, log);
    fn_set_property_string(player, "pause", "yes");
    resizeAndCapture(640, 360, "HDR reconfigure");
    DestroyPlayer(player);
    DestroyWindow(window);
}
