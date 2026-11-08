#if !defined(_WIN32)

#include "../MediaProbeIsolation.h"
#include "../Common.h"
#include "../App.h"
#include "../AppInternals.h"
#include "../resource.h"
#include "../IpcProtocol.h"
#include "../DllLoader.h"
#include "../Diagnostics.h"
#include "../Updater.h"
#include "Version.h"
#include "../Localization.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <fstream>
#include <memory>

using namespace Aperture;

extern void LinuxSetCommandLine(int argc, char** argv);

static App* g_pApp = nullptr;

LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CREATE) {
        CREATESTRUCTW* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        App* pApp = pCreate ? reinterpret_cast<App*>(pCreate->lpCreateParams) : nullptr;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pApp));
        DisableLegacyTouchGestures(hWnd);
        return 0;
    }
    if (uMsg == WM_TABLET_QUERYSYSTEMGESTURESTATUS) {
        return TabletSystemGestureStatus();
    }
    App* pApp = reinterpret_cast<App*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (pApp) {
        return pApp->HandleMessage(hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK VideoWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_TABLET_QUERYSYSTEMGESTURESTATUS) {
        return TabletSystemGestureStatus();
    }
    if (uMsg == WM_CREATE) {
        DisableLegacyTouchGestures(hWnd);
    }
    App* pApp = reinterpret_cast<App*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (pApp) {
        return pApp->HandleVideoMessage(hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

int main(int argc, char** argv) {
    LinuxSetCommandLine(argc, argv);

    int argCount = 0;
    LPWSTR* argList = CommandLineToArgvW(GetCommandLineW(), &argCount);
    if (Aperture::IsMediaProbeHelperCommand(argCount, argList)) {
        const int result = Aperture::RunMediaProbeHelper(argCount, argList);
        LocalFree(argList);
        return result;
    }
    if (argList && argCount > 1) {
        for (int i = 1; i < argCount; ++i) {
            if (std::wcscmp(argList[i], L"--update-helper") == 0) {
                int helperRes = Aperture::RunHelperMode(argCount, argList);
                LocalFree(argList);
                return helperRes;
            }
        }
    }
    if (argList) LocalFree(argList);

    Diagnostics::RecordHeartbeat();
    Diagnostics::RecordStartupMilestone(L"process-entry");

    LARGE_INTEGER qpcStart{}, qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcStart);

    DllLoader::HardenProcessDllSearch();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const wchar_t CLASS_NAME[] = L"Aperture_Native_Direct2D_Class";
    const wchar_t VIDEO_CLASS_NAME[] = L"Aperture_Native_Video_Child";

    CommandLineParseOutput cmdLineParse = Ipc::ParseCommandLine(GetCommandLineW());
    if (!cmdLineParse.IsSuccess()) {
        std::fprintf(stderr, "Aperture Command-Line Error: %s\n", Utf16ToUtf8(cmdLineParse.errorMessage).c_str());
        Renderer::ShutdownPrewarm();
        Diagnostics::Shutdown();
        return 1;
    }
    const PlaybackCommandLineOptions& initialOptions = cmdLineParse.options;

    if (initialOptions.mediaTargets.empty()) {
        Renderer::PrewarmD3DDeviceAsync();
    }

    Settings startupSettings;
    startupSettings.Load();
    Localization::Instance().Initialize(GetExeDir(), startupSettings.appLanguage);
    Diagnostics::Initialize(startupSettings.diagnosticsLogging);
    Diagnostics::RecordHeartbeat();
    Diagnostics::RecordStartupMilestone(L"settings-available");

    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(D2DColorToColorRef(startupSettings.currentTheme.bg));
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    RegisterClassExW(&wc);

    WNDCLASSEXW vc = {sizeof(WNDCLASSEXW)};
    vc.lpfnWndProc = VideoWndProc;
    vc.hInstance = hInstance;
    vc.lpszClassName = VIDEO_CLASS_NAME;
    vc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    RegisterClassExW(&vc);

    int winW = std::clamp(startupSettings.windowWidth, 640, 3840);
    int winH = std::clamp(startupSettings.windowHeight, 420, 2160);
    int winX = startupSettings.hasWindowPlacement ? startupSettings.windowX : 100;
    int winY = startupSettings.hasWindowPlacement ? startupSettings.windowY : 100;
    DWORD dwStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    DWORD dwExStyle = WS_EX_ACCEPTFILES;

    auto appStorage = std::make_unique<App>();
    App& app = *appStorage;
    g_pApp = &app;

    HWND hWnd = CreateWindowExW(dwExStyle, CLASS_NAME, L"Aperture", dwStyle, winX, winY, winW, winH, nullptr, nullptr, hInstance, &app);
    if (!hWnd) {
        Renderer::ShutdownPrewarm();
        Diagnostics::Shutdown();
        return -1;
    }

    Diagnostics::RecordStartupMilestone(L"window-created");
    HWND hVideoChild = CreateWindowExW(0, VIDEO_CLASS_NAME, nullptr, WS_CHILD | WS_CLIPSIBLINGS, 0, 0, winW, winH, hWnd, nullptr, hInstance, nullptr);

    {
        const auto& th = startupSettings.currentTheme;
        app.SetBootstrapColors(
            D2DColorToColorRef(th.bg),
            RGB(static_cast<BYTE>(th.text.r * 255.0f), static_cast<BYTE>(th.text.g * 255.0f), static_cast<BYTE>(th.text.b * 255.0f)),
            RGB(static_cast<BYTE>(th.textMuted.r * 255.0f), static_cast<BYTE>(th.textMuted.g * 255.0f), static_cast<BYTE>(th.textMuted.b * 255.0f)));
    }

    ShowWindow(hWnd, startupSettings.windowMaximized ? SW_MAXIMIZE : SW_SHOWNORMAL);
    Diagnostics::RecordStartupMilestone(L"window-shown");
    InvalidateRect(hWnd, nullptr, FALSE);
    UpdateWindow(hWnd);

    LARGE_INTEGER qpcShell{};
    QueryPerformanceCounter(&qpcShell);

    if (!app.Initialize(hWnd, hVideoChild, hInstance, initialOptions, &startupSettings)) {
        DestroyWindow(hWnd);
        Renderer::ShutdownPrewarm();
        Diagnostics::Shutdown();
        return -1;
    }

    InvalidateRect(hWnd, nullptr, FALSE);
    UpdateWindow(hWnd);
    Diagnostics::StartWatchdog();

    LARGE_INTEGER qpcNow{};
    QueryPerformanceCounter(&qpcNow);
    double elapsedMs = static_cast<double>(qpcNow.QuadPart - qpcStart.QuadPart) * 1000.0 / static_cast<double>(qpcFreq.QuadPart);
    double shownMs = static_cast<double>(qpcShell.QuadPart - qpcStart.QuadPart) * 1000.0 / static_cast<double>(qpcFreq.QuadPart);

    if (initialOptions.benchmarkProbe) {
        ULONGLONG deadline = GetTickCount64() + 10000;
        while (!app.HasRenderedLibrarySnapshot() && GetTickCount64() < deadline) {
            if (app.IsLibraryLoadComplete() && !app.IsLibraryReady()) break;
            MSG pending{};
            while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
                if (pending.message == WM_QUIT) break;
                TranslateMessage(&pending);
                DispatchMessageW(&pending);
            }
            if (app.Update(0.016f) || app.IsLibraryReady()) {
                InvalidateRect(hWnd, nullptr, FALSE);
                UpdateWindow(hWnd);
            }
            if (app.HasRenderedLibrarySnapshot()) break;
            Sleep(8);
        }
        LARGE_INTEGER qpcPop{};
        QueryPerformanceCounter(&qpcPop);
        double populatedMs = static_cast<double>(qpcPop.QuadPart - qpcStart.QuadPart) * 1000.0 / static_cast<double>(qpcFreq.QuadPart);
        std::printf("[APERTURE_BENCHMARK_PROBE_READY] startupMs=%.2f shownMs=%.2f populatedMs=%.2f libraryReady=%s\n",
                    elapsedMs, shownMs, populatedMs, app.IsLibraryReady() ? "true" : "false");
        DestroyWindow(hWnd);
        Renderer::ShutdownPrewarm();
        Diagnostics::Shutdown();
        return 0;
    }

    LARGE_INTEGER qpcLast{};
    QueryPerformanceCounter(&qpcLast);
    DWORD idleWaitMs = 16;
    MSG msg{};

    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                goto ExitApp;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        QueryPerformanceCounter(&qpcNow);
        float dt = static_cast<float>(static_cast<double>(qpcNow.QuadPart - qpcLast.QuadPart) / static_cast<double>(qpcFreq.QuadPart));
        qpcLast = qpcNow;
        if (dt < 0.0f) dt = 0.0f;

        if (g_pApp) {
            bool needsRedraw = g_pApp->Update(dt);
            if (needsRedraw) {
                g_pApp->Render();
            }
            if (needsRedraw) {
                idleWaitMs = 16;
            } else {
                idleWaitMs = std::min<DWORD>(64, idleWaitMs * 2);
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, needsRedraw ? 8 : idleWaitMs, QS_ALLINPUT);
        }
    }

ExitApp:
    Renderer::ShutdownPrewarm();
    Diagnostics::RecordHeartbeat();
    Diagnostics::Shutdown();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}

#endif // !defined(_WIN32)
