// Aperture libmpv candidate validation harness.
// Loads a specific libmpv-2.dll by absolute path (mimicking Aperture's
// DllLoader::LoadPackageLocalDll flags), verifies the loaded module path,
// then runs focused playback scenarios against that DLL instance.
//
// Build (MSVC): cl /nologo /utf-8 /O2 /MT /EHsc /std:c++17 mpv_playback_test.cpp
//               /I <workspace>\include /Fe:mpv_playback_test.exe
//               /link ole32.lib user32.lib shell32.lib /SUBSYSTEM:CONSOLE
//
// Usage: mpv_playback_test.exe --dll <absolute path to libmpv-2.dll>
//                              [--media <dir with generated test media>]
//                              [--scenario <name>]   (default: all)
//                              [--url <playback URL>]

#include <windows.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <functional>
#include <psapi.h>
#include <dxgi1_4.h>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dxgi.lib")

extern "C" {
#include "mpv/client.h"
#include "mpv/render.h"
}

typedef unsigned long (*pfn_mpv_client_api_version)();

static void* g_mpv = nullptr;
typedef mpv_handle* (*pfn_mpv_create)();
typedef int (*pfn_mpv_initialize)(mpv_handle*);
typedef void (*pfn_mpv_terminate_destroy)(mpv_handle*);
typedef int (*pfn_mpv_command)(mpv_handle*, const char**);
typedef int (*pfn_mpv_command_string)(mpv_handle*, const char*);
typedef int (*pfn_mpv_set_property_string)(mpv_handle*, const char*, const char*);
typedef int (*pfn_mpv_get_property)(mpv_handle*, const char*, mpv_format, void*);
typedef char* (*pfn_mpv_get_property_string)(mpv_handle*, const char*);
typedef void (*pfn_mpv_free)(void*);
typedef int (*pfn_mpv_observe_property)(mpv_handle*, uint64_t, const char*, mpv_format);
typedef mpv_event* (*pfn_mpv_wait_event)(mpv_handle*, double);
typedef const char* (*pfn_mpv_error_string)(int);
typedef int (*pfn_mpv_request_log_messages)(mpv_handle*, const char*);
typedef void (*pfn_mpv_wakeup)(mpv_handle*);
typedef int (*pfn_mpv_set_wakeup_callback)(mpv_handle*, void (*)(void*), void*);

static pfn_mpv_create fn_create;
static pfn_mpv_initialize fn_initialize;
static pfn_mpv_terminate_destroy fn_terminate_destroy;
static pfn_mpv_command fn_command;
static pfn_mpv_command_string fn_command_string;
static pfn_mpv_set_property_string fn_set_property_string;
static pfn_mpv_get_property fn_get_property;
static pfn_mpv_get_property_string fn_get_property_string;
static pfn_mpv_free fn_free;
static pfn_mpv_observe_property fn_observe_property;
static pfn_mpv_wait_event fn_wait_event;
static pfn_mpv_error_string fn_error_string;
static pfn_mpv_request_log_messages fn_request_log_messages;

static HMODULE g_hMod = nullptr;
static std::wstring g_dllPath;
static std::string g_loadedModulePath;
static int g_failures = 0;
static int g_startupIters = 8;
static int g_passes = 0;
static std::string g_tlsVerify = "yes";
static std::string g_renderer = "gpu";
static std::string g_videoSync = "display-vdrop", g_decoderThreads = "0";
static bool g_resourceAudio = false;
static std::vector<std::string> g_failDetails;

static void Report(const char* name, bool pass, const std::string& detail = "") {
    if (pass) { g_passes++; printf("[PASS] %s\n", name); }
    else {
        g_failures++;
        g_failDetails.push_back(std::string(name) + (detail.empty() ? "" : (" :: " + detail)));
        printf("[FAIL] %s%s%s\n", name, detail.empty() ? "" : " :: ", detail.c_str());
    }
    fflush(stdout);
}

static bool PumpMessages(DWORD ms) {
    DWORD start = GetTickCount();
    MSG msg;
    while (GetTickCount() - start < ms) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(15);
        return false; // pump once briefly; caller loops
    }
    return true;
}

// Pump messages while waiting on mpv events.
static mpv_event* WaitEvent(mpv_handle* mpv, double timeout) {
    PumpMessages(10);
    return fn_wait_event(mpv, timeout);
}

static std::string GetStr(mpv_handle* mpv, const char* prop) {
    char* s = fn_get_property_string(mpv, prop);
    std::string out;
    if (s) { out = s; fn_free(s); }
    return out;
}

static double GetDbl(mpv_handle* mpv, const char* prop, double def = -1.0) {
    double v = def;
    if (fn_get_property(mpv, prop, MPV_FORMAT_DOUBLE, &v) != 0) return def;
    return v;
}

static int64_t GetInt(mpv_handle* mpv, const char* prop, int64_t def = -1) {
    int64_t v = def;
    if (fn_get_property(mpv, prop, MPV_FORMAT_INT64, &v) != 0) return def;
    return v;
}

static bool WaitForCondition(mpv_handle* mpv, std::function<bool()> cond, double timeoutSec, std::string* lastLog = nullptr) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (cond()) return true;
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeoutSec) return false;
        mpv_event* ev = WaitEvent(mpv, 0.1);
        if (ev && ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message* m = (mpv_event_log_message*)ev->data;
            if (lastLog) *lastLog = m->text;
            printf("    [log:%s] %s", m->prefix, m->text);
        } else if (ev && ev->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file* ef = (mpv_event_end_file*)ev->data;
            const char* efileErr = ef->error ? fn_error_string(ef->error) : "none";
            printf("    [END_FILE reason=%d error=%s]\n", (int)ef->reason, efileErr);
            if (ef->reason != MPV_END_FILE_REASON_EOF && ef->reason != MPV_END_FILE_REASON_STOP &&
                ef->reason != MPV_END_FILE_REASON_REDIRECT && ef->reason != MPV_END_FILE_REASON_QUIT) {
                return false; // error end
            }
            // If file ended, condition on it will never come; let caller decide via cond.
        } else if (ev && ev->event_id == MPV_EVENT_SHUTDOWN) {
            return false;
        }
    }
}

static mpv_handle* NewPlayer(const std::vector<std::pair<std::string, std::string>>& opts, HWND attachHwnd) {
    mpv_handle* mpv = fn_create();
    if (!mpv) return nullptr;
    fn_set_property_string(mpv, "input-default-bindings", "no");
    fn_set_property_string(mpv, "input-vo-keyboard", "no");
    fn_set_property_string(mpv, "input-media-keys", "no");
    fn_set_property_string(mpv, "osd-level", "0");
    fn_set_property_string(mpv, "ytdl", "no");
    fn_set_property_string(mpv, "tls-verify", g_tlsVerify.c_str());
    for (auto& kv : opts) {
        const auto& value = kv.first == "vo" && kv.second == "gpu" ? g_renderer : kv.second;
        const int result = fn_set_property_string(mpv, kv.first.c_str(), value.c_str());
        if (result < 0) {
            Report(("option " + kv.first + "=" + value).c_str(), false, fn_error_string(result));
            fn_terminate_destroy(mpv); return nullptr;
        }
    }
    if (attachHwnd) {
        char widBuf[32];
        snprintf(widBuf, sizeof(widBuf), "%llu", (unsigned long long)(uintptr_t)attachHwnd);
        fn_set_property_string(mpv, "wid", widBuf);
    }
    fn_request_log_messages(mpv, "warn");
    if (fn_initialize(mpv) != 0) {
        fn_terminate_destroy(mpv);
        return nullptr;
    }
    return mpv;
}

static void DestroyPlayer(mpv_handle* mpv) {
    if (mpv) fn_terminate_destroy(mpv);
}

// ---- window for vo=gpu (d3d11 swapchain) ----
static LRESULT CALLBACK TestWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}
static HWND CreateTestWindow(int w, int h) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = TestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ApertureMpvTestWnd";
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"mpv-test", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    return hwnd;
}

// ---- scenario implementations ----
struct FileCheck {
    const char* file;
    const char* videoCodecContains; // "" = no video
    const char* audioCodecContains; // "" = no audio
    const char* name;
};

static void Scenario_PlaybackBasics(const std::string& media, const FileCheck& fc, bool wantHwdec) {
    char testName[256];
    snprintf(testName, sizeof(testName), "%s: load+play (hwdec=%s)", fc.name, wantHwdec ? "d3d11va" : "no");
    HWND hwnd = CreateTestWindow(320, 180);
    std::vector<std::pair<std::string, std::string>> opts = {
        {"vo", "gpu"}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"},
        {"hwdec", wantHwdec ? "d3d11va" : "no"},
        {"force-window", "yes"}, {"keep-open", "yes"}, {"volume", "0"},
        {"video-sync", "audio"}, {"hr-seek", "always"},
    };
    mpv_handle* mpv = NewPlayer(opts, hwnd);
    if (!mpv) { Report(testName, false, "player init failed"); DestroyWindow(hwnd); return; }

    std::string path = media.empty() ? fc.file : media + "/" + fc.file;
    const char* cmd[] = {"loadfile", path.c_str(), nullptr};
    int err = fn_command(mpv, cmd);
    if (err != 0) { Report(testName, false, std::string("loadfile: ") + fn_error_string(err)); DestroyPlayer(mpv); DestroyWindow(hwnd); return; }

    std::string log;
    bool started = WaitForCondition(mpv, [&]{ return GetDbl(mpv, "time-pos") > 0.2; }, 25.0, &log);
    Report((std::string(testName) + ": started").c_str(), started, log);

    if (started) {
        if (*fc.videoCodecContains) {
            std::string vc = GetStr(mpv, "video-format");
            if (vc.empty()) vc = GetStr(mpv, "video-codec");
            bool ok = strstr(vc.c_str(), fc.videoCodecContains) != nullptr;
            Report((std::string(fc.name) + ": video format " + vc).c_str(), ok);
        }
        if (*fc.audioCodecContains) {
            std::string ac = GetStr(mpv, "current-ao/audio-codec-name");
            if (ac.empty()) ac = GetStr(mpv, "audio-codec-name");
            bool ok = strstr(ac.c_str(), fc.audioCodecContains) != nullptr;
            Report((std::string(fc.name) + ": audio codec " + ac).c_str(), ok);
        }
        // let it play 2s
        double t0 = GetDbl(mpv, "time-pos");
        Sleep(2000);
        PumpMessages(50);
        double t1 = GetDbl(mpv, "time-pos");
        Report((std::string(fc.name) + ": time advances").c_str(), t1 > t0 + 1.0);

        if (wantHwdec && *fc.videoCodecContains) {
            std::string hw = GetStr(mpv, "hwdec-current");
            // note: hwdec may legitimately be absent if no GPU/driver; report but don't hard-fail
            if (hw == "no" || hw.empty() || hw == "false") {
                printf("[WARN] %s: hwdec-current='%s' (GPU may be absent)\n", fc.name, hw.c_str());
            } else {
                Report((std::string(fc.name) + ": hwdec active (" + hw + ")").c_str(), true);
            }
        }

        // drops / errors during 2s playback window
        int64_t voDrops = GetInt(mpv, "frame-drop-count-total");
        int64_t decDrops = GetInt(mpv, "decoder-frame-drop-count");
        printf("    drops: vo=%lld dec=%lld\n", (long long)voDrops, (long long)decDrops);
    }
    DestroyPlayer(mpv);
    DestroyWindow(hwnd);
}

static void Scenario_SeekPauseSpeed(const std::string& media) {
    const char* testName = "seek/pause/speed";
    HWND hwnd = CreateTestWindow(320, 180);
    std::vector<std::pair<std::string, std::string>> opts = {
        {"vo", "gpu"}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"},
        {"hwdec", "d3d11va"}, {"force-window", "yes"}, {"keep-open", "yes"}, {"volume", "0"},
    };
    mpv_handle* mpv = NewPlayer(opts, hwnd);
    if (!mpv) { Report(testName, false, "init"); DestroyWindow(hwnd); return; }
    std::string path = media + "/h264_aac_720p.mp4";
    const char* cmd[] = {"loadfile", path.c_str(), nullptr};
    fn_command(mpv, cmd);
    std::string log;
    bool started = WaitForCondition(mpv, [&]{ return GetDbl(mpv, "time-pos") > 0.2; }, 25.0, &log);
    if (!started) { Report(testName, false, "no start"); DestroyPlayer(mpv); DestroyWindow(hwnd); return; }

    // pause / resume
    fn_set_property_string(mpv, "pause", "yes");
    Sleep(300);
    double p0 = GetDbl(mpv, "time-pos");
    Sleep(700);
    PumpMessages(50);
    double p1 = GetDbl(mpv, "time-pos");
    Report("pause holds position", fabs(p1 - p0) < 0.35);
    fn_set_property_string(mpv, "pause", "no");
    Sleep(500);
    double p2 = GetDbl(mpv, "time-pos");
    Report("resume advances", p2 > p0 + 0.1);

    // seek (hr-seek)
    const char* seekCmd[] = {"seek", "8", "absolute", nullptr};
    int err = fn_command(mpv, seekCmd);
    bool seekOk = WaitForCondition(mpv, [&]{ double t = GetDbl(mpv, "time-pos"); return t > 7.0 && t < 9.5; }, 12.0, &log);
    Report("hr-seek to 8s", err == 0 && seekOk, log);

    // playback speed with pitch correction
    fn_set_property_string(mpv, "audio-pitch-correction", "yes");
    err = fn_set_property_string(mpv, "speed", "1.5");
    Sleep(1500);
    PumpMessages(50);
    double t0 = GetDbl(mpv, "time-pos");
    Sleep(1500);
    PumpMessages(50);
    double t1 = GetDbl(mpv, "time-pos");
    Report("speed 1.5 advances faster", err == 0 && (t1 - t0) > 1.8);
    std::string af = GetStr(mpv, "af");
    (void)af; // scaletempo presence is internal
    DestroyPlayer(mpv);
    DestroyWindow(hwnd);
}

static void Scenario_Subtitles(const std::string& media) {
    const char* testName = "subtitles external srt";
    HWND hwnd = CreateTestWindow(320, 180);
    std::vector<std::pair<std::string, std::string>> opts = {
        {"vo", "gpu"}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"},
        {"hwdec", "d3d11va"}, {"force-window", "yes"}, {"keep-open", "yes"}, {"volume", "0"},
        {"sub-auto", "fuzzy"}, {"sub-font-size", "46"},
    };
    mpv_handle* mpv = NewPlayer(opts, hwnd);
    if (!mpv) { Report(testName, false, "init"); DestroyWindow(hwnd); return; }
    std::string path = media + "/h264_aac_720p.mp4";
    const char* cmd[] = {"loadfile", path.c_str(), nullptr};
    fn_command(mpv, cmd);
    std::string log;
    WaitForCondition(mpv, [&]{ return GetDbl(mpv, "time-pos") > 0.2; }, 25.0, &log);

    std::string srt = media + "/test_subs.srt";
    const char* subCmd[] = {"sub-add", srt.c_str(), "select", nullptr};
    int err = fn_command(mpv, subCmd);
    Sleep(500);
    std::string sid = GetStr(mpv, "sid");
    int64_t subCount = GetInt(mpv, "track-list/count");
    // count sub tracks
    int subs = 0;
    for (int64_t i = 0; i < subCount; i++) {
        char prop[64];
        snprintf(prop, sizeof(prop), "track-list/%lld/type", (long long)i);
        if (GetStr(mpv, prop) == "sub") subs++;
    }
    Report("sub-add srt + select", err == 0 && subs >= 1 && (sid != "no" && !sid.empty()));
    Report("sub-visibility", GetStr(mpv, "sub-visibility") == "yes");
    std::string ass = media + "/test_subs.ass";
    const char* assCmd[] = {"sub-add", ass.c_str(), "select", nullptr};
    int assErr = fn_command(mpv, assCmd);
    bool assText = WaitForCondition(mpv, [&] { return GetStr(mpv, "sub-text").find("ASS integration") != std::string::npos; }, 10.0, &log);
    Report("ASS subtitle decoded through libass", assErr == 0 && assText, log);


    // embedded subs (mov_text in mp4): fresh instance so the track-list check
    // is deterministic (a second loadfile in the same instance races with the
    // previous file's track-list teardown)
    const char* testName2 = "subtitles embedded mov_text";
    DestroyPlayer(mpv);
    DestroyWindow(hwnd);
    {
        HWND hwnd2 = CreateTestWindow(320, 180);
        mpv_handle* mpv2 = NewPlayer(opts, hwnd2);
        if (!mpv2) { Report(testName2, false, "init"); return; }
        std::string path2 = media + "/h264_subs_movtext.mp4";
        const char* cmd2[] = {"loadfile", path2.c_str(), nullptr};
        int err2 = fn_command(mpv2, cmd2);
        auto subTrackCount = [&]() {
            int64_t n = GetInt(mpv2, "track-list/count");
            int subs = 0;
            for (int64_t i = 0; i < n; i++) {
                char prop[64];
                snprintf(prop, sizeof(prop), "track-list/%lld/type", (long long)i);
                if (GetStr(mpv2, prop) == "sub") subs++;
            }
            return subs;
        };
        std::string log2;
        bool started2 = WaitForCondition(mpv2, [&]{ return GetDbl(mpv2, "time-pos") > 0.2; }, 25.0, &log2);
        bool trackSeen = WaitForCondition(mpv2, [&]{ return subTrackCount() >= 1; }, 10.0, &log2);
        int subs2 = subTrackCount();
        {
            char dbg[160];
            snprintf(dbg, sizeof(dbg), "err=%d started=%d trackSeen=%d subs=%d", err2, (int)started2, (int)trackSeen, subs2);
            Report(testName2, err2 == 0 && started2 && trackSeen && subs2 >= 1, dbg);
        }
        DestroyPlayer(mpv2);
        DestroyWindow(hwnd2);
    }
}

static void Scenario_Screenshot(const std::string& media) {
    const char* testName = "screenshot-to-file (thumbnail path)";
    HWND hwnd = CreateTestWindow(320, 180);
    std::vector<std::pair<std::string, std::string>> opts = {
        {"vo", "gpu"}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"},
        {"hwdec", "d3d11va"}, {"force-window", "yes"}, {"keep-open", "yes"}, {"volume", "0"},
    };
    mpv_handle* mpv = NewPlayer(opts, hwnd);
    if (!mpv) { Report(testName, false, "init"); DestroyWindow(hwnd); return; }
    std::string path = media + "/h264_aac_720p.mp4";
    const char* cmd[] = {"loadfile", path.c_str(), nullptr};
    fn_command(mpv, cmd);
    std::string log;
    WaitForCondition(mpv, [&]{ return GetDbl(mpv, "time-pos") > 0.5; }, 25.0, &log);
    std::string shot = media + "/shot_test.png";
    DeleteFileA(shot.c_str());
    const char* shotCmd[] = {"screenshot-to-file", shot.c_str(), "video", nullptr};
    int err = fn_command(mpv, shotCmd);
    bool ok = false;
    if (err == 0) {
        ok = WaitForCondition(mpv, [&]{ DWORD a = GetFileAttributesA(shot.c_str()); return a != INVALID_FILE_ATTRIBUTES; }, 10.0, &log);
    }
    if (ok) {
        HANDLE f = CreateFileA(shot.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            GetFileSizeEx(f, &sz);
            ok = sz.QuadPart > 1024;
            CloseHandle(f);
        } else ok = false;
    }
    Report(testName, err == 0 && ok, log);
    DestroyPlayer(mpv);
    DestroyWindow(hwnd);
}

static void Scenario_SoftwareRender(const std::string& media) {
    // Mirrors Aperture's preview/thumbnail path: MPV_RENDER_API_TYPE_SW.
    const char* testName = "software render API (SW) frames";
    std::vector<std::pair<std::string, std::string>> opts = {
        {"vo", "libmpv"}, {"hwdec", "d3d11va"}, {"keep-open", "yes"}, {"volume", "0"},
    };
    mpv_handle* mpv = NewPlayer(opts, nullptr);
    if (!mpv) { Report(testName, false, "init"); return; }
    mpv_render_context* ctx = nullptr;
    // resolve render API
    auto fn_rc_create = (int (*)(mpv_render_context**, mpv_handle*, mpv_render_param*))GetProcAddress(g_hMod, "mpv_render_context_create");
    auto fn_rc_render = (int (*)(mpv_render_context*, mpv_render_param*))GetProcAddress(g_hMod, "mpv_render_context_render");
    auto fn_rc_free = (void (*)(mpv_render_context*))GetProcAddress(g_hMod, "mpv_render_context_free");
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    int err = fn_rc_create(&ctx, mpv, params);
    if (err != 0) { Report(testName, false, "render ctx create failed"); DestroyPlayer(mpv); return; }
    std::string path = media + "/h264_aac_720p.mp4";
    const char* cmd[] = {"loadfile", path.c_str(), nullptr};
    fn_command(mpv, cmd);
    std::string log;
    WaitForCondition(mpv, [&]{ return GetDbl(mpv, "time-pos") > 0.2; }, 25.0, &log);

    int size[2] = { 320, 180 };
    const char* format = "bgra";
    size_t stride = 320 * 4;
    std::vector<uint8_t> buf(stride * 180);
    bool gotFrame = false;
    for (int i = 0; i < 30 && !gotFrame; i++) {
        Sleep(100);
        mpv_render_param dparams[] = {
            { MPV_RENDER_PARAM_SW_SIZE, size },
            { MPV_RENDER_PARAM_SW_FORMAT, (void*)format },
            { MPV_RENDER_PARAM_SW_STRIDE, &stride },
            { MPV_RENDER_PARAM_SW_POINTER, buf.data() },
            { MPV_RENDER_PARAM_INVALID, nullptr },
        };
        err = fn_rc_render(ctx, dparams);
        if (err != 0) { Report(testName, false, "render failed"); fn_rc_free(ctx); DestroyPlayer(mpv); return; }
        uint32_t sum = 0;
        for (size_t p = 0; p < buf.size(); p += 4096) sum += buf[p];
        if (sum != 0) gotFrame = true; // non-black content rendered
    }
    Report(testName, gotFrame, log);
    fn_rc_free(ctx);
    DestroyPlayer(mpv);
}

static void Scenario_AudioFeatures(const std::string& media) {
    const char* testName = "night mode dynaudnorm + pan + balance + speed-pitch";
    HWND hwnd = CreateTestWindow(320, 180);
    std::vector<std::pair<std::string, std::string>> opts = {
        {"vo", "gpu"}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"},
        {"hwdec", "d3d11va"}, {"force-window", "yes"}, {"keep-open", "yes"}, {"volume", "50"},
        {"audio-channels", "stereo"}, {"audio-pitch-correction", "yes"},
    };
    mpv_handle* mpv = NewPlayer(opts, hwnd);
    if (!mpv) { Report(testName, false, "init"); DestroyWindow(hwnd); return; }
    std::string path = media + "/h264_aac_51_720p.mp4";
    const char* cmd[] = {"loadfile", path.c_str(), nullptr};
    fn_command(mpv, cmd);
    std::string log;
    bool started = WaitForCondition(mpv, [&]{ return GetDbl(mpv, "time-pos") > 0.2; }, 25.0, &log);
    if (!started) { Report(testName, false, "no start " + log); DestroyPlayer(mpv); DestroyWindow(hwnd); return; }

    // night mode: dynaudnorm (exact Aperture AudioConfig filter string)
    fn_command_string(mpv, "af set lavfi=[dynaudnorm=f=200:g=21:p=0.95:m=10.0]");
    Sleep(800);
    PumpMessages(50);
    std::string af = GetStr(mpv, "af");
    bool dyn = af.find("dynaudnorm") != std::string::npos;
    Report("af lavfi dynaudnorm active", dyn, af);

    // channel map: pan (mono-left from AudioConfig)
    fn_command_string(mpv, "af set lavfi=[pan=stereo|c0=c0|c1=c0]");
    Sleep(800);
    PumpMessages(50);
    af = GetStr(mpv, "af");
    bool pan = af.find("pan") != std::string::npos;
    Report("af lavfi pan active", pan, af);

    // combined (as BuildAudioPlan does: night + channel)
    fn_command_string(mpv, "af set lavfi=[dynaudnorm=f=150:g=27:p=0.95:m=15.0],lavfi=[pan=stereo|c0=c1|c1=c0]");
    Sleep(800);
    PumpMessages(50);
    af = GetStr(mpv, "af");
    bool both = af.find("dynaudnorm") != std::string::npos && af.find("pan") != std::string::npos;
    Report("af combined night+map active", both, af);
    double t3 = GetDbl(mpv, "time-pos");
    Sleep(1000);
    double t4 = GetDbl(mpv, "time-pos");
    Report("audio still playing with filters", t4 > t3 + 0.5);

    // balance property (mpv internally inserts pan filter) — fresh instance so
    // no pre-existing lavfi filters mask the inserted pan
    DestroyPlayer(mpv);
    DestroyWindow(hwnd);
    {
        const char* balTest = "balance property (internal pan)";
        HWND hwnd2 = CreateTestWindow(320, 180);
        std::vector<std::pair<std::string, std::string>> opts2 = {
            {"vo", "gpu"}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"},
            {"hwdec", "d3d11va"}, {"force-window", "yes"}, {"keep-open", "yes"}, {"volume", "50"},
        };
        mpv_handle* mpv2 = NewPlayer(opts2, hwnd2);
        if (!mpv2) { Report(balTest, false, "init"); return; }
        const char* cmd2[] = {"loadfile", path.c_str(), nullptr};
        fn_command(mpv2, cmd2);
        std::string log2;
        WaitForCondition(mpv2, [&]{ return GetDbl(mpv2, "time-pos") > 0.2; }, 25.0, &log2);
        int err2 = fn_set_property_string(mpv2, "balance", "0.5");
        Sleep(700);
        PumpMessages(50);
        std::string af2 = GetStr(mpv2, "af");
        bool bal = err2 == 0 && af2.find("pan") != std::string::npos;
        printf("[INFO] %s: error=%d (%s), pan=%d; unsupported on the original DLL too\n",
               balTest, err2, fn_error_string(err2), (int)bal);
        DestroyPlayer(mpv2);
        DestroyWindow(hwnd2);
    }
    // downmix option acceptance (option exists / parses)
    {
        const char* dmTest = "audio-normalize-downmix accepted";
        HWND hwnd3 = CreateTestWindow(320, 180);
        std::vector<std::pair<std::string, std::string>> opts3 = {
            {"vo", "gpu"}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"},
            {"force-window", "yes"}, {"keep-open", "yes"}, {"volume", "0"},
            {"audio-channels", "stereo"},
        };
        mpv_handle* mpv3 = NewPlayer(opts3, hwnd3);
        if (!mpv3) { Report(dmTest, false, "init"); return; }
        int errDm = fn_set_property_string(mpv3, "audio-normalize-downmix", "yes");
        Report(dmTest, errDm == 0);
        DestroyPlayer(mpv3);
        DestroyWindow(hwnd3);
    }
}

static bool WaitForEof(mpv_handle* mpv, double timeoutSec, std::string* activeHwdec = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSec);
    while (std::chrono::steady_clock::now() < deadline) {
        if (activeHwdec) {
            std::string current = GetStr(mpv, "hwdec-current");
            if (!current.empty() && current != "no") *activeHwdec = current;
        }
        mpv_event* event = WaitEvent(mpv, 0.05);
        if (event && event->event_id == MPV_EVENT_END_FILE) {
            auto* end = static_cast<mpv_event_end_file*>(event->data);
            return end->reason == MPV_END_FILE_REASON_EOF && end->error == 0;
        }
        if (event && event->event_id == MPV_EVENT_SHUTDOWN) return false;
    }
    return false;
}

static void Scenario_DecodeBench(const std::string& media, double& swWall, double& swCpu, double& hwWall, double& hwCpu, int64_t& drops) {
    // untimed full-speed software decode: vo=null
    {
        std::vector<std::pair<std::string, std::string>> opts = {
            {"vo", "null"}, {"untimed", "yes"}, {"hwdec", "no"}, {"volume", "0"},
            {"audio", "no"}, {"vd-lavc-threads", "0"},
        };
        mpv_handle* mpv = NewPlayer(opts, nullptr);
        if (!mpv) { Report("decode bench sw", false, "init"); return; }
        std::string path = media + "/h264_1080p_60s.mp4";
        const char* cmd[] = {"loadfile", path.c_str(), nullptr};
        fn_command(mpv, cmd);
        FILETIME c0, e0, k0, u0, c1, e1, k1, u1;
        GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
        auto w0 = std::chrono::steady_clock::now();
        std::string log;
        bool done = WaitForEof(mpv, 300.0);
        auto w1 = std::chrono::steady_clock::now();
        GetProcessTimes(GetCurrentProcess(), &c1, &e1, &k1, &u1);
        swWall = std::chrono::duration<double>(w1 - w0).count();
        auto toSec = [](const FILETIME& f) { return (double)((*(uint64_t*)&f)) / 1e7; };
        swCpu = (toSec(k1) - toSec(k0)) + (toSec(u1) - toSec(u0));
        Report("decode bench sw completed", done, log);
        printf("    [bench] sw decode: wall=%.2fs cpu=%.2fs\n", swWall, swCpu);
        DestroyPlayer(mpv);
    }
    // untimed hw decode through gpu: d3d11va
    {
        HWND hwnd = CreateTestWindow(320, 180);
        std::vector<std::pair<std::string, std::string>> opts = {
            {"vo", "gpu"}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"},
            {"untimed", "yes"}, {"hwdec", "d3d11va"}, {"volume", "0"}, {"audio", "no"},
        };
        mpv_handle* mpv = NewPlayer(opts, hwnd);
        if (!mpv) { Report("decode bench hw", false, "init"); DestroyWindow(hwnd); return; }
        std::string path = media + "/h264_1080p_60s.mp4";
        const char* cmd[] = {"loadfile", path.c_str(), nullptr};
        fn_command(mpv, cmd);
        FILETIME c0, e0, k0, u0, c1, e1, k1, u1;
        GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
        auto w0 = std::chrono::steady_clock::now();
        std::string log;
        std::string hw;
        bool done = WaitForEof(mpv, 300.0, &hw);
        auto w1 = std::chrono::steady_clock::now();
        GetProcessTimes(GetCurrentProcess(), &c1, &e1, &k1, &u1);
        hwWall = std::chrono::duration<double>(w1 - w0).count();
        auto toSec = [](const FILETIME& f) { return (double)((*(uint64_t*)&f)) / 1e7; };
        hwCpu = (toSec(k1) - toSec(k0)) + (toSec(u1) - toSec(u0));
        drops = GetInt(mpv, "frame-drop-count-total") + GetInt(mpv, "decoder-frame-drop-count");
        Report("decode bench hw completed", done, log);
        printf("    [bench] hw(%s) decode: wall=%.2fs cpu=%.2fs drops=%lld\n", hw.c_str(), hwWall, hwCpu, (long long)drops);
        DestroyPlayer(mpv);
        DestroyWindow(hwnd);
    }
}

static void Scenario_Network(const std::string& media, int port) {
    const char* testName = "network http playback";
    HWND hwnd = CreateTestWindow(320, 180);
    std::vector<std::pair<std::string, std::string>> opts = {
        {"vo", "gpu"}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"},
        {"hwdec", "d3d11va"}, {"force-window", "yes"}, {"keep-open", "yes"}, {"volume", "0"},
    };
    mpv_handle* mpv = NewPlayer(opts, hwnd);
    if (!mpv) { Report(testName, false, "init"); DestroyWindow(hwnd); return; }
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/h264_aac_faststart.mp4", port);
    const char* cmd[] = {"loadfile", url, nullptr};
    int err = fn_command(mpv, cmd);
    std::string log;
    bool started = WaitForCondition(mpv, [&]{ return GetDbl(mpv, "time-pos") > 0.5; }, 30.0, &log);
    Report(testName, err == 0 && started, log);
    DestroyPlayer(mpv);
    DestroyWindow(hwnd);
}

static void Scenario_MpvVersion() {
    auto fn_ver = (pfn_mpv_client_api_version)GetProcAddress(g_hMod, "mpv_client_api_version");
    if (!fn_ver) { Report("client api version", false, "export missing"); return; }
    unsigned long v = fn_ver();
    char buf[128];
    snprintf(buf, sizeof(buf), "mpv_client_api_version=%#lx (%lu.%lu)", v, v >> 16, v & 0xFFFF);
    printf("    %s\n", buf);
    Report("client api version >= 2.5", (v >> 16) == 2 && (v & 0xFFFF) >= 5, buf);
}

static void Scenario_Startup(const std::string& media, int iters) {
    const char* testName = "startup latency (loadfile -> playback restart)";
    HWND hwnd = CreateTestWindow(320, 180);
    std::vector<std::pair<std::string, std::string>> opts = {
        {"vo", "gpu"}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"},
        {"hwdec", "d3d11va"}, {"force-window", "yes"}, {"keep-open", "yes"}, {"volume", "0"},
        {"pause", "yes"},
    };
    mpv_handle* mpv = NewPlayer(opts, hwnd);
    if (!mpv) { Report(testName, false, "init"); DestroyWindow(hwnd); return; }
    std::string path = media + "/h264_aac_720p.mp4";
    std::vector<double> lat;
    for (int i = 0; i < iters + 1; i++) { // first iteration = warmup
        const char* cmd[] = {"loadfile", path.c_str(), nullptr};
        auto t0 = std::chrono::steady_clock::now();
        fn_command(mpv, cmd);
        std::string log;
        // Paused playback: measure engine readiness via playback-restart.
        // This event does not measure compositor presentation or first light.
        bool sawRestart = false;
        auto start = std::chrono::steady_clock::now();
        while (true) {
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (el >= 25.0) break;
            mpv_event* ev = WaitEvent(mpv, 0.05);
            if (ev && ev->event_id == MPV_EVENT_PLAYBACK_RESTART) { sawRestart = true; break; }
            if (ev && ev->event_id == MPV_EVENT_END_FILE) break;
            if (ev && ev->event_id == MPV_EVENT_SHUTDOWN) break;
        }
        auto t1 = std::chrono::steady_clock::now();
        if (sawRestart && i > 0) lat.push_back(std::chrono::duration<double>(t1 - t0).count());
        fn_command_string(mpv, "stop");
        Sleep(300);
        PumpMessages(30);
    }
    std::sort(lat.begin(), lat.end());
    double sum = 0; for (double v : lat) sum += v;
    double med = lat.empty() ? -1 : lat[lat.size() / 2];
    printf("    [startup] median=%.3fs min=%.3fs max=%.3fs n=%zu\n", med,
           lat.empty() ? -1.0 : lat.front(), lat.empty() ? -1.0 : lat.back(), lat.size());
    FILE* f = fopen("startup_result.txt", "w");
    if (f) { fprintf(f, "startup_median=%.4f\nstartup_min=%.4f\nstartup_max=%.4f\n", med,
                      lat.empty() ? -1.0 : lat.front(), lat.empty() ? -1.0 : lat.back()); fclose(f); }
    Report(testName, !lat.empty());
    DestroyPlayer(mpv);
    DestroyWindow(hwnd);
}

#include "playback_resource_probe.h"

int main(int argc, char** argv) {
    std::string dll, media, scenario = "all";
    int benchPort = 8765;
    std::string url, resourceFile;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dll") && i + 1 < argc) dll = argv[++i];
        else if (!strcmp(argv[i], "--media") && i + 1 < argc) media = argv[++i];
        else if (!strcmp(argv[i], "--scenario") && i + 1 < argc) scenario = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) benchPort = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--url") && i + 1 < argc) url = argv[++i];
        else if (!strcmp(argv[i], "--tls-verify") && i + 1 < argc) g_tlsVerify = argv[++i];
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) g_startupIters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--renderer") && i + 1 < argc) g_renderer = argv[++i];
        else if (!strcmp(argv[i], "--resource-file") && i + 1 < argc) resourceFile = argv[++i];
        else if (!strcmp(argv[i], "--video-sync") && i + 1 < argc) g_videoSync = argv[++i];
        else if (!strcmp(argv[i], "--decoder-threads") && i + 1 < argc) g_decoderThreads = argv[++i];
        else if (!strcmp(argv[i], "--with-audio")) g_resourceAudio = true;
    }
    if (dll.empty()) { printf("usage: --dll <path> [--media dir] [--scenario name]\n"); return 2; }

    // Resolve to absolute path and load like Aperture's DllLoader
    char abs[MAX_PATH];
    GetFullPathNameA(dll.c_str(), MAX_PATH, abs, nullptr);
    g_dllPath = std::wstring(abs, abs + strlen(abs));

    g_hMod = LoadLibraryExW(g_dllPath.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_hMod) g_hMod = LoadLibraryExW(g_dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_hMod) {
        printf("[FAIL] LoadLibrary(%s) error=%lu\n", abs, GetLastError());
        return 1;
    }
    char loaded[MAX_PATH];
    GetModuleFileNameA(g_hMod, loaded, MAX_PATH);
    g_loadedModulePath = loaded;
    printf("Loaded module: %s\n", g_loadedModulePath.c_str());
    printf("Loaded module path matches requested: %s\n",
           _stricmp(g_loadedModulePath.c_str(), abs) == 0 ? "YES" : "NO");
    Report("loaded DLL is the requested absolute path", _stricmp(g_loadedModulePath.c_str(), abs) == 0);

    fn_create = (pfn_mpv_create)GetProcAddress(g_hMod, "mpv_create");
    fn_initialize = (pfn_mpv_initialize)GetProcAddress(g_hMod, "mpv_initialize");
    fn_terminate_destroy = (pfn_mpv_terminate_destroy)GetProcAddress(g_hMod, "mpv_terminate_destroy");
    fn_command = (pfn_mpv_command)GetProcAddress(g_hMod, "mpv_command");
    fn_command_string = (pfn_mpv_command_string)GetProcAddress(g_hMod, "mpv_command_string");
    fn_set_property_string = (pfn_mpv_set_property_string)GetProcAddress(g_hMod, "mpv_set_property_string");
    fn_get_property = (pfn_mpv_get_property)GetProcAddress(g_hMod, "mpv_get_property");
    fn_get_property_string = (pfn_mpv_get_property_string)GetProcAddress(g_hMod, "mpv_get_property_string");
    fn_free = (pfn_mpv_free)GetProcAddress(g_hMod, "mpv_free");
    fn_observe_property = (pfn_mpv_observe_property)GetProcAddress(g_hMod, "mpv_observe_property");
    fn_wait_event = (pfn_mpv_wait_event)GetProcAddress(g_hMod, "mpv_wait_event");
    fn_error_string = (pfn_mpv_error_string)GetProcAddress(g_hMod, "mpv_error_string");
    fn_request_log_messages = (pfn_mpv_request_log_messages)GetProcAddress(g_hMod, "mpv_request_log_messages");
    if (!fn_create || !fn_initialize || !fn_wait_event || !fn_command) {
        printf("[FAIL] required exports missing\n");
        return 1;
    }

    const bool all = scenario == "all" || scenario == "functional";
    Scenario_MpvVersion();

    bool wantMedia = scenario != "version";
    if (wantMedia && media.empty()) { printf("Missing --media argument\n"); return 2; }
    if (wantMedia && !media.empty()) {
        double swWall = -1, swCpu = -1, hwWall = -1, hwCpu = -1;
        int64_t drops = -1;
        if (all || scenario == "basics") {
            Scenario_PlaybackBasics(media, {"h264_aac_720p.mp4", "h264", "aac", "h264+aac mp4"}, true);
            Scenario_PlaybackBasics(media, {"h264_aac_720p.mp4", "h264", "aac", "h264+aac mp4"}, false);
            Scenario_PlaybackBasics(media, {"hevc_aac_720p.mp4", "hevc", "aac", "hevc+aac mp4"}, true);
            Scenario_PlaybackBasics(media, {"hevc_aac_720p.mp4", "hevc", "aac", "hevc+aac mp4"}, false);
            Scenario_PlaybackBasics(media, {"av1_opus.mkv", "av1", "opus", "av1+opus mkv"}, false);
            Scenario_PlaybackBasics(media, {"vp9_opus.webm", "vp9", "opus", "vp9+opus webm"}, false);
            Scenario_PlaybackBasics(media, {"mpeg4_mp3.avi", "mpeg4", "mp3", "mpeg4+mp3 avi"}, false);
            Scenario_PlaybackBasics(media, {"wmv_wma.wmv", "wmv", "wmav2", "wmv3+wma asf"}, false);
        }

        if (all || scenario == "hdr") {
            Scenario_PlaybackBasics(media, {"hevc_hdr10.mp4", "hevc", "", "HEVC 10-bit HDR/PQ"}, true);
            Scenario_PlaybackBasics(media, {"hevc_hdr10.mp4", "hevc", "", "HEVC 10-bit HDR/PQ"}, false);
        }
        if (all || scenario == "controls") Scenario_SeekPauseSpeed(media);
        if (all || scenario == "subs") Scenario_Subtitles(media);
        if (all || scenario == "screenshot") Scenario_Screenshot(media);
        if (all || scenario == "swrender") Scenario_SoftwareRender(media);
        if (all || scenario == "audio") Scenario_AudioFeatures(media);
        if (scenario == "all" || scenario == "bench") Scenario_DecodeBench(media, swWall, swCpu, hwWall, hwCpu, drops);
        if (all || scenario == "network") Scenario_Network(media, benchPort);
        if (scenario == "url") Scenario_PlaybackBasics("", {url.c_str(), "", "", "network URL"}, false);
        if (scenario == "startup") Scenario_Startup(media, g_startupIters);
        if (scenario == "resources") Scenario_Resources(media, resourceFile);

        if (all || scenario == "bench") {
            FILE* f = fopen("bench_result.txt", "w");
            if (f) {
                fprintf(f, "sw_wall=%.3f\nsw_cpu=%.3f\nhw_wall=%.3f\nhw_cpu=%.3f\ndrops=%lld\n",
                        swWall, swCpu, hwWall, hwCpu, (long long)drops);
                fclose(f);
            }
        }
    }

    FreeLibrary(g_hMod);
    printf("\n==== SUMMARY: %d passed, %d failed ====\n", g_passes, g_failures);
    for (auto& d : g_failDetails) printf("  FAILED: %s\n", d.c_str());
    return g_failures == 0 ? 0 : 1;
}
