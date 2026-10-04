#include "OverlayRenderer.h"
#include "../unit/TestOverlayPacing.h"
#include <iostream>
#include <iomanip>

// No fixtures or profile stores. This probes the production DC/DIB/layered
// renderer with representative controls, rather than mpv decode or power use.
namespace {
double Clock() {
    static const double frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return double(value.QuadPart); }();
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now); return double(now.QuadPart) / frequency;
}
double CpuMilliseconds() {
    FILETIME create{}, exit{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user);
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return double(k.QuadPart + u.QuadPart) / 10000.0;
}
LRESULT CALLBACK ProbeProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(window, message, wp, lp);
}
void Pump() { MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } }
void Draw(Aperture::OverlayRenderer& renderer, HWND window, bool subtitleOnly, double elapsed, UINT width, UINT height) {
    renderer.BeginDraw();
    if (subtitleOnly) {
        renderer.DrawSubtitle(L"A stable subtitle cue", D2D1::RectF(0, 0, float(width), float(height)), "box",
            D2D1::ColorF(1.0f, 1.0f, 1.0f), 28.0f, "bottom");
    } else {
        const float w = float(width), h = float(height);
        renderer.DrawString(L"Aperture playback overlay", D2D1::RectF(60, 16, w - 24, 52), D2D1::ColorF(1.0f, 1.0f, 1.0f), Aperture::FontType::Title);
        renderer.FillCircle(42, 34, 18, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f));
        renderer.DrawIcon(Aperture::IconType::ChevronLeft, D2D1::RectF(32, 24, 52, 44), D2D1::ColorF(1.0f, 1.0f, 1.0f));
        renderer.FillRoundedRect(D2D1::RectF(24, h - 62, w - 24, h - 58), 2.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f));
        renderer.FillRoundedRect(D2D1::RectF(24, h - 62, 24 + (w - 48) * float((300.0 + elapsed) / 1200.0), h - 58), 2.0f,
            D2D1::ColorF(0.4f, 0.65f, 1.0f));
        renderer.DrawIcon(Aperture::IconType::Pause, D2D1::RectF(24, h - 40, 46, h - 18), D2D1::ColorF(1.0f, 1.0f, 1.0f));
        renderer.DrawString(Aperture::FormatDuration(300.0 + elapsed) + L" / 20:00", D2D1::RectF(60, h - 44, 220, h - 12),
            D2D1::ColorF(1.0f, 1.0f, 1.0f), Aperture::FontType::Mono);
    }
    renderer.EndDrawAndPresent(window, 0, 0);
}
int ValidateNative(Aperture::OverlayRenderer& renderer, HWND window, ID2D1Factory1* d2d, IDWriteFactory* dwrite) {
    renderer.Resize(320, 180);
    if (!renderer.Initialize(d2d, dwrite, float(GetDpiForWindow(window)) / 96.0f) ||
        !renderer.GetFrameLatencyWaitableObject() || !renderer.BeginFrame(window)) return 10;
    if (renderer.BeginFrame(window) || !renderer.HasPendingFrame()) return 11; // Immediate duplicate input is coalesced.
    Draw(renderer, window, false, 0.0, 320, 180);
    if (renderer.GetFrameStatistics().framesPresented != 1 || !renderer.HasPendingFrame()) return 12;
    if (WaitForSingleObject(renderer.GetFrameLatencyWaitableObject(), 100) != WAIT_OBJECT_0) return 13;
    renderer.RequestRedraw();
    if (!renderer.IsFrameDue()) return 14;
    renderer.BeginDraw();
    if (renderer.EndDrawAndPresent(nullptr, 0, 0) || !renderer.HasPendingFrame() || renderer.GetFrameStatistics().presentationFailures != 1) return 15;
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(data)->push_back(monitor); return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));
    for (auto monitor : monitors) {
        MONITORINFOEXW info{}; info.cbSize = sizeof(info);
        DEVMODEW mode{}; mode.dmSize = sizeof(mode);
        if (!GetMonitorInfoW(monitor, &info)) return 16;
        SetWindowPos(window, nullptr, info.rcMonitor.left, info.rcMonitor.top, 320, 180, SWP_NOZORDER | SWP_NOACTIVATE);
        Sleep(20);
        if (!renderer.BeginFrame(window)) return 17;
        const auto stats = renderer.GetFrameStatistics();
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency >= 20 && mode.dmDisplayFrequency <= 360) {
            if (stats.displayRefreshRate != mode.dmDisplayFrequency || stats.refreshRate != std::min(60.0, double(mode.dmDisplayFrequency))) return 18;
        }
    }
    renderer.Shutdown();
    DWORD handlesBefore{}, handlesAfter{};
    GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
    const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int cycle = 0; cycle < 8; ++cycle) {
        if (!renderer.Initialize(d2d, dwrite)) return 19;
        renderer.Shutdown();
    }
    GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
    const DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    if (handlesAfter != handlesBefore || gdiAfter != gdiBefore) return 20;
    std::cout << "{\"nativeInitialization\":true,\"inputCoalescing\":true,\"timerDeadline\":true,\"presentationFailureRetries\":true,"
        << "\"monitorsChecked\":" << monitors.size() << ",\"crossMonitorAvailable\":" << (monitors.size() > 1 ? "true" : "false")
        << ",\"handlesBefore\":" << handlesBefore << ",\"handlesAfter\":" << handlesAfter
        << ",\"gdiBefore\":" << gdiBefore << ",\"gdiAfter\":" << gdiAfter << "}\n";
    return 0;
}
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--test") {
        int failed = 0;
        for (const auto& test : Aperture::Test::TestRegistry::Instance().GetTests()) {
            const auto before = Aperture::Test::TestRegistry::Instance().FailureCount();
            try {
                test.fn();
                if (Aperture::Test::TestRegistry::Instance().FailureCount() == before) std::cout << "PASS " << test.suite << "::" << test.name << "\n";
                else { ++failed; std::cerr << "FAIL " << test.suite << "::" << test.name << "\n"; }
            }
            catch (const std::exception& error) { ++failed; std::cerr << error.what() << "\n"; }
        }
        return failed ? 1 : 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2d;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) return 2;
    WNDCLASSW wc{}; wc.lpfnWndProc = ProbeProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ApertureOverlayCadenceProbe";
    RegisterClassW(&wc);
    constexpr UINT width = 3840, height = 2160;
    HWND window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        wc.lpszClassName, L"Aperture overlay cadence probe", WS_POPUP, 0, 0, width, height, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return 3;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    if (argc > 1 && std::string(argv[1]) == "--native-validation") {
        Aperture::OverlayRenderer renderer;
        const int result = ValidateNative(renderer, window, d2d.Get(), dwrite.Get());
        DestroyWindow(window);
        CoUninitialize();
        return result;
    }
    std::cout << "mode,sample,wall_ms,cpu_ms,frames,frames_per_second,draw_ms,present_ms,present_failures,overlay_hz,display_hz,surface_bytes,desktop_width,desktop_height\n";
    for (const std::string mode : {"legacy_controls", "paced_controls", "legacy_subtitle", "content_subtitle", "hidden_controls"}) {
        for (int sample = 0; sample < 3; ++sample) {
            Aperture::OverlayRenderer renderer;
            renderer.Resize(width, height);
            if (!renderer.Initialize(d2d.Get(), dwrite.Get())) return 4;
            const bool paced = mode == "paced_controls";
            const bool subtitle = mode == "legacy_subtitle" || mode == "content_subtitle";
            // Exclude lazy DirectWrite/device initialization from the interval.
            renderer.BeginFrame(window);
            Draw(renderer, window, subtitle, 0.0, width, height);
            renderer.ShouldRefreshPlayback(false, subtitle ? L"A stable subtitle cue" : L"");
            Sleep(30);
            const auto before = renderer.GetFrameStatistics();
            const double cpuStart = CpuMilliseconds(), start = Clock();
            while (Clock() - start < 2.0) {
                Pump();
                bool redraw = mode != "hidden_controls";
                if (mode == "content_subtitle") redraw = renderer.ShouldRefreshPlayback(false, L"A stable subtitle cue");
                if (redraw && (!paced || renderer.BeginFrame(window))) Draw(renderer, window, subtitle, Clock() - start, width, height);
                HANDLE timer = paced ? renderer.GetFrameLatencyWaitableObject() : nullptr;
                if (timer) MsgWaitForMultipleObjectsEx(1, &timer, 1000, QS_ALLINPUT, 0);
                else MsgWaitForMultipleObjects(0, nullptr, FALSE, mode == "hidden_controls" || mode == "content_subtitle" ? 64 : 8, QS_ALLINPUT);
            }
            const double elapsedMs = (Clock() - start) * 1000.0, cpuMs = CpuMilliseconds() - cpuStart;
            const auto after = renderer.GetFrameStatistics();
            const auto frames = after.framesPresented - before.framesPresented;
            std::cout << mode << ',' << sample << ',' << std::fixed << std::setprecision(3) << elapsedMs << ',' << cpuMs << ',' << frames << ','
                << frames * 1000.0 / elapsedMs << ',' << after.drawMilliseconds - before.drawMilliseconds << ','
                << after.presentMilliseconds - before.presentMilliseconds << ',' << after.presentationFailures - before.presentationFailures << ','
                << after.refreshRate << ',' << after.displayRefreshRate << ',' << after.surfaceBytes << ',' << GetSystemMetrics(SM_CXSCREEN) << ',' << GetSystemMetrics(SM_CYSCREEN) << '\n';
        }
    }
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
