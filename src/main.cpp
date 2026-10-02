#include "Common.h"
#include "App.h"
#include "AppInternals.h"
#include "resource.h"
#include "IpcProtocol.h"
#include "DllLoader.h"
#include "Diagnostics.h"
#include "Updater.h"
#include "Version.h"
#include "Localization.h"
#include <psapi.h>

using namespace Aperture;

static App* g_pApp = nullptr;

LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        App* pApp = reinterpret_cast<App*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pApp));
        DisableLegacyTouchGestures(hWnd);
        return 0;
    }
    if (uMsg == WM_TABLET_QUERYSYSTEMGESTURESTATUS) { return TabletSystemGestureStatus(); }
    App* pApp = reinterpret_cast<App*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
    if (pApp) {
        if (uMsg == WM_COPYDATA) {
            PCOPYDATASTRUCT pCDS = reinterpret_cast<PCOPYDATASTRUCT>(lParam);
            if (!pCDS || pCDS->cbData == 0 || pCDS->cbData > Ipc::MAX_IPC_PAYLOAD_SIZE) { return FALSE; }
            if (pCDS->dwData == Ipc::IPC_MAGIC_V2) {
                if (pCDS->cbData < sizeof(Ipc::EnvelopeHeader)) { return FALSE; }
                const Ipc::EnvelopeHeader* peekHeader = reinterpret_cast<const Ipc::EnvelopeHeader*>(pCDS->lpData);
                if (peekHeader->magic != Ipc::IPC_MAGIC_V2) { return FALSE; }
                if (peekHeader->command == static_cast<uint32_t>(Ipc::Command::Automation)) {
                    Ipc::AutomationCommand autoCmd;
                    Ipc::EnvelopeHeader header = {};
                    Ipc::ValidationResult res = Ipc::DeserializeAutomationEnvelope(
                        pCDS->lpData, pCDS->cbData, autoCmd, &header, true /* authenticateSender */, true /* checkReplay */
                    );
                    if (res != Ipc::ValidationResult::Success) { return FALSE; }
                    Ipc::AutomationStatus status = {};
                    bool success = pApp->ExecuteAutomationCommand(autoCmd, &status);
                    if (!success) { return FALSE; }
                    if (autoCmd.op == Ipc::AutomationOp::QueryStatus) {
                        HWND hSenderWnd = reinterpret_cast<HWND>(wParam);
                        if (hSenderWnd && IsWindow(hSenderWnd)) {
                            DWORD senderPid = 0;
                            GetWindowThreadProcessId(hSenderWnd, &senderPid);
                            if (senderPid == header.senderPid && Ipc::IsSameUserAndSession(senderPid)) {
                                std::vector<uint8_t> replyEnv = Ipc::SerializeAutomationResponse(status);
                                if (!replyEnv.empty()) {
                                    COPYDATASTRUCT replyCds = {};
                                    replyCds.dwData = Ipc::IPC_MAGIC_V2;
                                    replyCds.cbData = static_cast<DWORD>(replyEnv.size());
                                    replyCds.lpData = replyEnv.data();
                                    DWORD_PTR dwRes = 0;
                                    SendMessageTimeoutW(hSenderWnd, WM_COPYDATA, reinterpret_cast<WPARAM>(hWnd), reinterpret_cast<LPARAM>(&replyCds), SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &dwRes);
                                }
                            }
                        }
                    }
                    if (header.flags & 1) {
                        if (IsIconic(hWnd)) { ShowWindow(hWnd, SW_RESTORE); }
                        SetForegroundWindow(hWnd);
                    }
                    return TRUE;
                } else {
                    PlaybackCommandLineOptions options;
                    Ipc::EnvelopeHeader header = {};
                    Ipc::ValidationResult res = Ipc::DeserializeEnvelope(
                        pCDS->lpData, pCDS->cbData, options, &header, true /* authenticateSender */
                    );
                    if (res != Ipc::ValidationResult::Success) { return FALSE; }
                    pApp->ExecuteCommandLineOptions(options);
                    if (header.flags & 1) {
                        if (IsIconic(hWnd)) { ShowWindow(hWnd, SW_RESTORE); }
                        SetForegroundWindow(hWnd);
                    }
                    return TRUE;
                }
            } else if (pCDS->dwData == Ipc::IPC_LEGACY_MAGIC) {
                if (pCDS->cbData < sizeof(wchar_t) || (pCDS->cbData % sizeof(wchar_t)) != 0) { return FALSE; }
                return pApp->HandleMessage(hWnd, uMsg, wParam, lParam);
            }
            return FALSE;
        }
        return pApp->HandleMessage(hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK VideoWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_TABLET_QUERYSYSTEMGESTURESTATUS) { return TabletSystemGestureStatus(); }
    if (uMsg == WM_CREATE) { DisableLegacyTouchGestures(hWnd); }
    App* pApp = reinterpret_cast<App*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
    if (pApp) { return pApp->HandleVideoMessage(hWnd, uMsg, wParam, lParam); }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    // Check if invoked in update-helper mode before mutex acquisition or UI creation
    int argCount = 0;
    LPWSTR* argList = CommandLineToArgvW(GetCommandLineW(), &argCount);
    if (argList && argCount > 1) {
        for (int i = 1; i < argCount; ++i) {
            if (wcscmp(argList[i], L"--update-helper") == 0) {
                int helperRes = Aperture::RunHelperMode(argCount, argList);
                LocalFree(argList);
                return helperRes;
            }
        }
        LocalFree(argList);
    }
    Diagnostics::RecordHeartbeat(L"process-start");
    Diagnostics::RecordStartupMilestone(L"process-entry");
    // 1. High precision timer for cold startup measurement
    LARGE_INTEGER qpcStart, qpcFreq;
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcStart);
    // 2. High-DPI Per-Monitor V2 Awareness & DLL Search Hardening
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    DllLoader::HardenProcessDllSearch();
    // 3. Initialize COM
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    const wchar_t CLASS_NAME[] = L"Aperture_Native_Direct2D_Class";
    // The libmpv child surface uses a private class with a black brush so it can never flash the STATIC control's white COLOR_WINDOW
    // background before the first video frame arrives.
    const wchar_t VIDEO_CLASS_NAME[] = L"Aperture_Native_Video_Child";
    // 4. Parse Command Line Arguments
    CommandLineParseOutput cmdLineParse = Ipc::ParseCommandLine(GetCommandLineW());
    if (!cmdLineParse.IsSuccess()) {
        MessageBoxW(NULL, cmdLineParse.errorMessage.c_str(), L"Aperture Command-Line Error", MB_ICONERROR | MB_OK);
        Renderer::ShutdownPrewarm();
        Diagnostics::Shutdown();
        CoUninitialize();
        return 1;
    }
    const PlaybackCommandLineOptions& initialOptions = cmdLineParse.options;
    // 5. Single-Instance Coordination via Mutex & IPC
    HANDLE hSingleInstanceMutex = NULL;
    std::unique_ptr<void, decltype(&CloseHandle)> windowReadyEvent(nullptr, &CloseHandle);
    if (!initialOptions.benchmarkProbe && !initialOptions.visualCaptureProbe) {
        hSingleInstanceMutex = CreateMutexW(NULL, FALSE, L"Local\\Aperture_SingleInstance_Mutex_V1");
        if (hSingleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND hExistingWnd = NULL;
            const ULONGLONG readinessDeadline = GetTickCount64() + 10000;
            while (GetTickCount64() < readinessDeadline) {
                std::unique_ptr<void, decltype(&CloseHandle)> ready(OpenEventW(SYNCHRONIZE, FALSE, L"Local\\Aperture_WindowReady_V1"), &CloseHandle);
                if (!ready || WaitForSingleObject(ready.get(), 40) == WAIT_OBJECT_0) {
                    hExistingWnd = FindWindowW(CLASS_NAME, NULL);
                    if (hExistingWnd) break;
                }
                Sleep(40);
            }
            bool delivered = false;
            if (hExistingWnd) {
                std::vector<uint8_t> envelope = Ipc::SerializeEnvelope(initialOptions, Ipc::Command::OpenFiles, 1);
                if (!envelope.empty()) {
                    COPYDATASTRUCT cds = {};
                    cds.dwData = Ipc::IPC_MAGIC_V2;
                    cds.cbData = static_cast<DWORD>(envelope.size());
                    cds.lpData = envelope.data();
                    DWORD_PTR dwResult = 0;
                    delivered = SendMessageTimeoutW(hExistingWnd, WM_COPYDATA, (WPARAM)NULL, (LPARAM)&cds,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &dwResult) != 0 && dwResult != FALSE;
                }
                if (IsIconic(hExistingWnd)) { ShowWindow(hExistingWnd, SW_RESTORE); }
                SetForegroundWindow(hExistingWnd);
            }
            CloseHandle(hSingleInstanceMutex);
            Renderer::ShutdownPrewarm();
            CoUninitialize();
            if (!delivered) {
                MessageBoxW(nullptr, L"The running Aperture window could not accept this request. Wait for it to respond, then try again.",
                    L"Aperture", MB_OK | MB_ICONWARNING);
                return ERROR_TIMEOUT;
            }
            return 0;
        }
        windowReadyEvent.reset(CreateEventW(nullptr, TRUE, FALSE, L"Local\\Aperture_WindowReady_V1"));
        if (windowReadyEvent) ResetEvent(windowReadyEvent.get());
    }
    // Prewarm Direct3D 11 device asynchronously on worker thread immediately
    // so driver initialization overlaps window setup, fonts, and library loading.
    Renderer::PrewarmD3DDeviceAsync();
    // Private fonts are loaded through DirectWrite custom collections in
    // Renderer/OverlayRenderer; GDI FR_PRIVATE loads were never visible to
    // DirectWrite text or mpv's font provider, so they are skipped here.
    HICON hIconBig = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON hIconSmall = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    // Load persisted placement/theme before registering the classes so the
    // background brush matches the active theme from the very first erase.
    Settings startupSettings;
    startupSettings.Load();
    Localization::Instance().Initialize(GetExeDir(), startupSettings.appLanguage);
    Diagnostics::Initialize(startupSettings.diagnosticsLogging);
    Diagnostics::RecordHeartbeat(L"settings-loaded");
    Diagnostics::RecordStartupMilestone(L"settings-available");
    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = hIconBig;
    wc.hIconSm = hIconSmall;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    // Paint the theme background for any erase our handler does not cover, so the first visible frame is the theme color rather than white.
    wc.hbrBackground = CreateSolidBrush(D2DColorToColorRef(startupSettings.currentTheme.bg));
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    RegisterClassEx(&wc);
    WNDCLASSEX vc = { sizeof(WNDCLASSEX) };
    vc.lpfnWndProc = VideoWndProc;
    vc.hInstance = hInstance;
    vc.lpszClassName = VIDEO_CLASS_NAME;
    vc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    vc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    RegisterClassEx(&vc);
    // Placement was loaded before class registration so the first non-client/client layout is restored atomically. Coordinates are
    // persisted in physical pixels by the per-monitor-v2 process and validated against the current monitors.
    RECT workArea{};
    RECT savedRect{};
    if (startupSettings.hasWindowPlacement) {
        savedRect.left = startupSettings.windowX;
        savedRect.top = startupSettings.windowY;
        savedRect.right = savedRect.left + std::max(1, startupSettings.windowWidth);
        savedRect.bottom = savedRect.top + std::max(1, startupSettings.windowHeight);
    } else {
        savedRect = { 0, 0, 1280, 780 };
    }
    HMONITOR monitor = MonitorFromRect(&savedRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        monitorInfo.rcWork = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }
    workArea = monitorInfo.rcWork;
    int workW = std::max(640, (int)(workArea.right - workArea.left));
    int workH = std::max(420, (int)(workArea.bottom - workArea.top));
    int winW = std::clamp(startupSettings.windowWidth, 640, workW);
    int winH = std::clamp(startupSettings.windowHeight, 420, workH);
    int winX = startupSettings.hasWindowPlacement ? savedRect.left : workArea.left + (workW - winW) / 2;
    int winY = startupSettings.hasWindowPlacement ? savedRect.top : workArea.top + (workH - winH) / 2;
    // Keep a usable caption strip visible even when a monitor was removed.
    winX = std::clamp(winX, (int)workArea.left - winW + 64, (int)workArea.right - 64);
    winY = std::clamp(winY, (int)workArea.top, (int)workArea.bottom - 64);
    DWORD dwStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, dwExStyle = WS_EX_ACCEPTFILES;
    if (initialOptions.visualCaptureProbe) {
        RECT targetClient = { 0, 0, 1024, 640 };
        AdjustWindowRectEx(&targetClient, dwStyle, FALSE, dwExStyle);
        winW = targetClient.right - targetClient.left;
        winH = targetClient.bottom - targetClient.top;
        winX = workArea.left + 50;
        winY = workArea.top + 50;
    }
    auto appStorage = std::make_unique<App>();
    App& app = *appStorage;
    g_pApp = &app;
    HWND hWnd = CreateWindowEx( dwExStyle, CLASS_NAME, L"Aperture", dwStyle, winX, winY, winW, winH, NULL, NULL, hInstance, &app );
    if (hWnd && initialOptions.visualCaptureProbe) {
        RECT targetClient = { 0, 0, 1024, 640 };
        AdjustWindowRectEx(&targetClient, dwStyle, FALSE, dwExStyle);
        SetWindowPos(hWnd, NULL, winX, winY, targetClient.right - targetClient.left, targetClient.bottom - targetClient.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!hWnd) {
        if (hSingleInstanceMutex) { CloseHandle(hSingleInstanceMutex); hSingleInstanceMutex = NULL; }
        Renderer::ShutdownPrewarm();
        Diagnostics::Shutdown();
        CoUninitialize();
        return -1;
    }
    ChangeWindowMessageFilterEx(hWnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);
    Diagnostics::RecordStartupMilestone(L"window-created");
    // 5. Windows 10 & 11 Dark Mode Titlebar
    BOOL useDarkMode = TRUE;
    if (FAILED(DwmSetWindowAttribute(hWnd, 20, &useDarkMode, sizeof(useDarkMode))))
        DwmSetWindowAttribute(hWnd, 19, &useDarkMode, sizeof(useDarkMode));
    SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
    HWND hVideoChild = CreateWindowEx(0, VIDEO_CLASS_NAME, NULL, WS_CHILD | WS_CLIPSIBLINGS, 0, 0, winW, winH, hWnd, NULL, hInstance, NULL);
    // Bootstrap shell before subsystem init: show the existing window and paint GDI first pixels (theme bg + title) without waiting for
    // library or D3D. App is not yet initialized, so WM_PAINT takes the GDI bootstrap branch (no Render/D3D). Input arriving during
    // Initialize stays queued.
    {
        const auto& th = startupSettings.currentTheme;
        app.SetBootstrapColors(D2DColorToColorRef(th.bg),
            RGB((BYTE)(th.text.r * 255.0f), (BYTE)(th.text.g * 255.0f), (BYTE)(th.text.b * 255.0f)),
            RGB((BYTE)(th.textMuted.r * 255.0f), (BYTE)(th.textMuted.g * 255.0f), (BYTE)(th.textMuted.b * 255.0f)));
    }
    ShowWindow(hWnd, startupSettings.windowMaximized ? SW_MAXIMIZE : nCmdShow);
    Diagnostics::RecordStartupMilestone(L"window-shown");
    InvalidateRect(hWnd, NULL, FALSE);
    UpdateWindow(hWnd);
    LARGE_INTEGER qpcShell;
    QueryPerformanceCounter(&qpcShell);
    // Shell pixels are up; the bootstrap-shell milestone itself is recorded
    // by App's first GDI bootstrap paint (in-memory only, no disk writes on normal runs).
    // 6. Initialize App subsystems (library, D3D join happens on first GPU
    // paint below, MF/SMTC/taskbar/tooltips stay lazy).
    if (!app.Initialize(hWnd, hVideoChild, hInstance, initialOptions, &startupSettings)) {
        if (!initialOptions.benchmarkProbe && !initialOptions.visualCaptureProbe)
            MessageBox(hWnd, L"Failed to initialize DirectX 11 / Direct2D / Media rendering pipeline.", L"Aperture Error", MB_ICONERROR);
        DestroyWindow(hWnd);
        if (hSingleInstanceMutex) { CloseHandle(hSingleInstanceMutex); hSingleInstanceMutex = NULL; }
        Renderer::ShutdownPrewarm();
        Diagnostics::Shutdown();
        CoUninitialize();
        return -1;
    }
    if (windowReadyEvent) SetEvent(windowReadyEvent.get());
    // GPU first frame after init: invalidates over the shell with the same theme colors, so the handoff has no flicker. UpdateWindow blocks
    // until the GPU frame presents (joining the D3D worker if needed).
    InvalidateRect(hWnd, NULL, FALSE);
    UpdateWindow(hWnd);
    // Crash-filter protection was active since process start; the hang
    // watchdog thread is deliberately deferred until after the first present
    // so it never contends with D3D/device/library work on the critical path.
    Diagnostics::StartWatchdog();
    // Resource sampling is reserved for the completed benchmark below: thread
    // enumeration here would delay adoption of the loaded library snapshot.
    // Shell vs full: qpcShell is GDI first pixels, qpcNow is the first GPU present. startup_perf.txt and the probe keep their field names,
    // with shownMs now honestly reporting shell latency.
    LARGE_INTEGER qpcNow;
    QueryPerformanceCounter(&qpcNow);
    double elapsedMs = (double)(qpcNow.QuadPart - qpcStart.QuadPart) * 1000.0 / (double)qpcFreq.QuadPart;
    double shownMs = (double)(qpcShell.QuadPart - qpcStart.QuadPart) * 1000.0 / (double)qpcFreq.QuadPart;
    std::wstring logPath = GetDataDir() + L"\\startup_perf.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, logPath.c_str(), L"w") == 0 && f) {
        fwprintf(f, L"%s Native Direct2D Startup: %.2f ms (window shown: %.2f ms)\n", std::wstring(Version::ProductNameWide).c_str(), elapsedMs, shownMs);
        fclose(f);
    }
    // Probe/capture modes do not enter the normal message loop. Pump the same UI messages here so the asynchronous library package can be
    // adopted and painted without turning production startup back into a blocking load.
    auto pumpUntilLibrarySnapshot = [&app, hWnd](DWORD timeoutMs) {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (!app.HasRenderedLibrarySnapshot()) {
            if (app.IsLibraryLoadComplete() && !app.IsLibraryReady()) break;
            MSG pending{};
            while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
                if (pending.message == WM_QUIT) return false;
                TranslateMessage(&pending);
                DispatchMessageW(&pending);
            }
            const bool redraw = app.Update(0.016f);
            if (redraw || app.IsLibraryReady()) { InvalidateRect(hWnd, nullptr, FALSE); UpdateWindow(hWnd); }
            if (app.HasRenderedLibrarySnapshot()) break;
            if (GetTickCount64() >= deadline) return false;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 8, QS_ALLINPUT);
        }
        return app.HasRenderedLibrarySnapshot();
    };
    if (initialOptions.benchmarkProbe) {
        const bool populated = pumpUntilLibrarySnapshot(10000);
        const bool probeReady = populated && app.IsLibraryReady();
        LARGE_INTEGER qpcPopulated{};
        QueryPerformanceCounter(&qpcPopulated);
        const double populatedMs = populated ? (double)(qpcPopulated.QuadPart - qpcStart.QuadPart) * 1000.0 / (double)qpcFreq.QuadPart
            : -1.0;
        Diagnostics::RecordStartupMilestone(L"benchmark-populated-resources", true);
        // Phase-level report for root-local A/B runs (diagnostics/perf/).
        Diagnostics::WriteStartupReport();
        PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof(PROCESS_MEMORY_COUNTERS_EX) };
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
        DWORD gdiObjects = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        DWORD userObjects = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hStdOut && hStdOut != INVALID_HANDLE_VALUE) {
                char buf[768];
                int len = sprintf_s(buf,
                    "[APERTURE_BENCHMARK_PROBE_READY] startupMs=%.2f shownMs=%.2f populatedMs=%.2f libraryReady=%s workingSetBytes=%zu peakWorkingSetBytes=%zu privateBytes=%zu peakPrivateBytes=%zu gdiObjects=%u userObjects=%u\n",
                    elapsedMs, shownMs, populatedMs, app.IsLibraryReady() ? "true" : "false", static_cast<size_t>(pmc.WorkingSetSize),
                    static_cast<size_t>(pmc.PeakWorkingSetSize), static_cast<size_t>(pmc.PrivateUsage),
                    static_cast<size_t>(pmc.PeakPagefileUsage), static_cast<unsigned int>(gdiObjects),
                    static_cast<unsigned int>(userObjects) );
                DWORD written = 0;
                WriteFile(hStdOut, buf, static_cast<DWORD>(len), &written, NULL);
            }
            FreeConsole();
        }
        if (!initialOptions.benchmarkOutputFile.empty()) {
            FILE* fOut = nullptr;
            if (_wfopen_s(&fOut, initialOptions.benchmarkOutputFile.c_str(), L"w") == 0 && fOut) {
                fwprintf(fOut, L"{\n"
                     L"  \"ready\": %ls,\n"
                     L"  \"startupMs\": %.2f,\n"
                     L"  \"shownMs\": %.2f,\n"
                     L"  \"populatedMs\": %.2f,\n"
                     L"  \"libraryReady\": %ls,\n"
                     L"  \"workingSetBytes\": %zu,\n"
                    L"  \"peakWorkingSetBytes\": %zu,\n"
                    L"  \"privateBytes\": %zu,\n"
                    L"  \"peakPrivateBytes\": %zu,\n"
                    L"  \"gdiObjects\": %u,\n"
                    L"  \"userObjects\": %u\n"
                    L"}\n", probeReady ? L"true" : L"false", elapsedMs, shownMs, populatedMs, app.IsLibraryReady() ? L"true" : L"false",
                    static_cast<size_t>(pmc.WorkingSetSize), static_cast<size_t>(pmc.PeakWorkingSetSize),
                    static_cast<size_t>(pmc.PrivateUsage), static_cast<size_t>(pmc.PeakPagefileUsage),
                    static_cast<unsigned int>(gdiObjects), static_cast<unsigned int>(userObjects) );
                fclose(fOut);
            }
        }
        DestroyWindow(hWnd);
        if (hSingleInstanceMutex) { CloseHandle(hSingleInstanceMutex); hSingleInstanceMutex = NULL; }
        Renderer::ShutdownPrewarm();
        Diagnostics::RecordHeartbeat(L"benchmark-probe-exit");
        Diagnostics::Shutdown();
        CoUninitialize();
        return 0;
    }
    if (initialOptions.visualCaptureProbe) {
        pumpUntilLibrarySnapshot(10000);
        if (!initialOptions.visualCaptureScene.empty()) {
            const std::wstring& scene = initialOptions.visualCaptureScene;
            if (scene == L"media" || scene == L"hub_media") {
                app.SetTab(TabView::Media);
            } else if (scene == L"playlists" || scene == L"hub_playlists") {
                app.SetTab(TabView::Playlists);
            } else if (scene == L"history" || scene == L"hub_history") {
                app.SetTab(TabView::History);
            } else if (scene == L"settings" || scene == L"hub_settings" || scene == L"settings_appearance") {
                app.SetSettingsCategory(SettingsCategory::Appearance);
            } else if (scene == L"settings_playback") {
                app.SetSettingsCategory(SettingsCategory::Playback);
            } else if (scene == L"settings_subtitles") {
                app.SetSettingsCategory(SettingsCategory::Subtitles);
            } else if (scene == L"settings_shortcuts") {
                app.SetSettingsCategory(SettingsCategory::Shortcuts);
            } else if (scene == L"settings_system") { app.SetSettingsCategory(SettingsCategory::System); }
        }
        app.Render();
        app.PrepareVisualCaptureScene(initialOptions.visualCaptureScene);
        app.Render();
        bool captureSuccess = false;
        if (!initialOptions.visualCaptureOutputFile.empty())
            captureSuccess = app.CaptureClientContent(initialOptions.visualCaptureOutputFile);
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hStdOut && hStdOut != INVALID_HANDLE_VALUE) {
                char buf[512];
                int len = sprintf_s(buf, "[APERTURE_VISUAL_CAPTURE_READY] scene=%ls success=%s output=%ls\n",
                    initialOptions.visualCaptureScene.empty() ? L"default" : initialOptions.visualCaptureScene.c_str(),
                    captureSuccess ? "true" : "false", initialOptions.visualCaptureOutputFile.c_str() );
                DWORD written = 0;
                WriteFile(hStdOut, buf, static_cast<DWORD>(len), &written, NULL);
            }
            FreeConsole();
        }
        DestroyWindow(hWnd);
        if (hSingleInstanceMutex) { CloseHandle(hSingleInstanceMutex); hSingleInstanceMutex = NULL; }
        Renderer::ShutdownPrewarm();
        Diagnostics::RecordHeartbeat(L"visual-capture-probe-exit");
        Diagnostics::Shutdown();
        CoUninitialize();
        return captureSuccess ? 0 : 1;
    }
    // 7. Event-driven message loop. A short wait caps idle polling while
    // retaining responsive animation/playback updates when the app is active. When nothing requests redraws and no input arrives, the
    // timeout backs off progressively so a fully idle hub idles at ~16 wakeups/s instead
    // of ~60; any message or redraw request drops it back to 16 ms.
    LARGE_INTEGER qpcLast;
    QueryPerformanceCounter(&qpcLast);
    DWORD idleWaitMs = 16;
    MSG msg = {};
    while (true) {
        // Process all pending Windows messages without blocking
        bool processedMessages = false;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            processedMessages = true;
            if (msg.message == WM_QUIT) goto ExitApp;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        // Measure true delta time
        QueryPerformanceCounter(&qpcNow);
        float dt = (float)((double)(qpcNow.QuadPart - qpcLast.QuadPart) / (double)qpcFreq.QuadPart);
        qpcLast = qpcNow;
        if (dt > 0.05f) dt = 0.05f;
        if (dt < 0.0f) dt = 0.0f;
        if (g_pApp) {
            bool needsRedraw = g_pApp->Update(dt);
            if (needsRedraw) { InvalidateRect(hWnd, NULL, FALSE); UpdateWindow(hWnd); }
            if (needsRedraw || processedMessages) { idleWaitMs = 16; } else { idleWaitMs = std::min<DWORD>(64, idleWaitMs * 2); }
            HANDLE hWaitable = g_pApp->GetFrameLatencyWaitableObject();
            if (needsRedraw) {
                if (g_pApp->IsOccluded()) { MsgWaitForMultipleObjects(0, NULL, FALSE, 16, QS_ALLINPUT); } else if (hWaitable) {
                    MsgWaitForMultipleObjectsEx(1, &hWaitable, 1000, QS_ALLINPUT, 0);
                } else { MsgWaitForMultipleObjects(0, NULL, FALSE, 8, QS_ALLINPUT); }
            } else { MsgWaitForMultipleObjects(0, NULL, FALSE, idleWaitMs, QS_ALLINPUT); }
        }
    }

ExitApp:
    if (hSingleInstanceMutex) { CloseHandle(hSingleInstanceMutex); hSingleInstanceMutex = NULL; }
    Renderer::ShutdownPrewarm();
    Diagnostics::RecordHeartbeat(L"process-exit");
    Diagnostics::Shutdown();
    CoUninitialize();
    return (int)msg.wParam;
}
