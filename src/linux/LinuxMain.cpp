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
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <fstream>
#include <memory>

using namespace Aperture;

extern void LinuxSetCommandLine(int argc, char** argv);

static App* g_pApp = nullptr;
static volatile sig_atomic_t g_linuxTermSignal = 0;

static void LinuxTermSignalHandler(int /*sig*/) {
    g_linuxTermSignal = 1;
    LinuxWakeMessageLoop();
}

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
        if (uMsg == WM_COPYDATA) {
            if (pApp->IsClosing()) return FALSE;
            PCOPYDATASTRUCT pCDS = reinterpret_cast<PCOPYDATASTRUCT>(lParam);
            if (!pCDS || !pCDS->lpData || pCDS->cbData == 0 || pCDS->cbData > Ipc::MAX_IPC_PAYLOAD_SIZE) {
                return FALSE;
            }
            if (pCDS->dwData == Ipc::IPC_MAGIC_V2) {
                if (pCDS->cbData < sizeof(Ipc::EnvelopeHeader)) return FALSE;
                const Ipc::EnvelopeHeader* peekHeader = reinterpret_cast<const Ipc::EnvelopeHeader*>(pCDS->lpData);
                if (peekHeader->magic != Ipc::IPC_MAGIC_V2) return FALSE;
                if (peekHeader->command == static_cast<uint32_t>(Ipc::Command::Automation)) {
                    Ipc::AutomationCommand autoCmd;
                    Ipc::EnvelopeHeader header = {};
                    if (Ipc::DeserializeAutomationEnvelope(pCDS->lpData, pCDS->cbData, autoCmd, &header, true, true) !=
                        Ipc::ValidationResult::Success) {
                        return FALSE;
                    }
                    Ipc::AutomationStatus status = {};
                    if (!pApp->ExecuteAutomationCommand(autoCmd, &status)) return FALSE;
                    if (autoCmd.op == Ipc::AutomationOp::QueryStatus) {
                        HWND hSenderWnd = reinterpret_cast<HWND>(wParam);
                        if (hSenderWnd && IsWindow(hSenderWnd)) {
                            std::vector<uint8_t> replyEnv = Ipc::SerializeAutomationResponse(status);
                            if (!replyEnv.empty()) {
                                COPYDATASTRUCT replyCds = {};
                                replyCds.dwData = Ipc::IPC_MAGIC_V2;
                                replyCds.cbData = static_cast<DWORD>(replyEnv.size());
                                replyCds.lpData = replyEnv.data();
                                DWORD_PTR dwRes = 0;
                                SendMessageTimeoutW(hSenderWnd, WM_COPYDATA, reinterpret_cast<WPARAM>(hWnd),
                                                    reinterpret_cast<LPARAM>(&replyCds), SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                                    2000, &dwRes);
                            }
                        }
                    }
                    if (header.flags & 1) {
                        if (IsIconic(hWnd)) ShowWindow(hWnd, SW_RESTORE);
                        SetForegroundWindow(hWnd);
                    }
                    return TRUE;
                }
                PlaybackCommandLineOptions options;
                Ipc::EnvelopeHeader header = {};
                if (Ipc::DeserializeEnvelope(pCDS->lpData, pCDS->cbData, options, &header, true) !=
                    Ipc::ValidationResult::Success) {
                    return FALSE;
                }
                pApp->ExecuteCommandLineOptions(options);
                if (header.flags & 1) {
                    if (IsIconic(hWnd)) ShowWindow(hWnd, SW_RESTORE);
                    SetForegroundWindow(hWnd);
                }
                return TRUE;
            }
            if (pCDS->dwData == Ipc::IPC_LEGACY_MAGIC) {
                if (pCDS->cbData < sizeof(wchar_t) || (pCDS->cbData % sizeof(wchar_t)) != 0) return FALSE;
                return pApp->HandleMessage(hWnd, uMsg, wParam, lParam);
            }
            return FALSE;
        }
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

    HANDLE hSingleInstanceMutex = nullptr;
    HANDLE hWindowReadyEvent = nullptr;
    if (!initialOptions.benchmarkProbe && !initialOptions.visualCaptureProbe) {
        hSingleInstanceMutex = CreateMutexW(nullptr, FALSE, L"Local\\Aperture_SingleInstance_Mutex_V1");
        if (hSingleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND hExistingWnd = nullptr;
            const ULONGLONG readinessDeadline = GetTickCount64() + 10000;
            while (GetTickCount64() < readinessDeadline) {
                HANDLE ready = OpenEventW(SYNCHRONIZE, FALSE, L"Local\\Aperture_WindowReady_V1");
                if (!ready || WaitForSingleObject(ready, 40) == WAIT_OBJECT_0) {
                    if (ready) CloseHandle(ready);
                    hExistingWnd = FindWindowW(CLASS_NAME, nullptr);
                    if (hExistingWnd) break;
                } else {
                    CloseHandle(ready);
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
                    delivered = SendMessageTimeoutW(hExistingWnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
                                                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &dwResult) != 0 &&
                                dwResult != FALSE;
                }
                if (IsIconic(hExistingWnd)) ShowWindow(hExistingWnd, SW_RESTORE);
                SetForegroundWindow(hExistingWnd);
            }
            CloseHandle(hSingleInstanceMutex);
            Renderer::ShutdownPrewarm();
            Diagnostics::Shutdown();
            CoUninitialize();
            return delivered ? 0 : 1;
        }
        hWindowReadyEvent = CreateEventW(nullptr, TRUE, FALSE, L"Local\\Aperture_WindowReady_V1");
    }

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
        if (hWindowReadyEvent) CloseHandle(hWindowReadyEvent);
        if (hSingleInstanceMutex) CloseHandle(hSingleInstanceMutex);
        Renderer::ShutdownPrewarm();
        Diagnostics::Shutdown();
        return -1;
    }

    Diagnostics::RecordStartupMilestone(L"window-created");
    if (hWindowReadyEvent) SetEvent(hWindowReadyEvent);
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
    MSG msg{};

    {
        LinuxWakeMessageLoop();
        struct sigaction termSa{};
        termSa.sa_handler = LinuxTermSignalHandler;
        sigemptyset(&termSa.sa_mask);
        termSa.sa_flags = SA_RESTART;
        ::sigaction(SIGINT, &termSa, nullptr);
        ::sigaction(SIGTERM, &termSa, nullptr);
    }

    while (true) {
        if (g_linuxTermSignal) {
            g_linuxTermSignal = 0;
            PostMessageW(hWnd, WM_CLOSE, 0, 0);
        }
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
            DWORD waitMs = g_pApp->GetNextIdleWaitMs();
            if (needsRedraw || g_pApp->HasPendingPlaybackFrame()) {
                if (g_pApp->IsOccluded()) {
                    waitMs = 16;
                } else if (g_pApp->HasPendingPlaybackFrame()) {
                    waitMs = g_pApp->GetPlaybackFrameWaitMs();
                } else {
                    waitMs = 0;
                }
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLINPUT);
        }
    }

ExitApp:
    if (hWindowReadyEvent) CloseHandle(hWindowReadyEvent);
    if (hSingleInstanceMutex) CloseHandle(hSingleInstanceMutex);
    Renderer::ShutdownPrewarm();
    Diagnostics::RecordHeartbeat();
    Diagnostics::Shutdown();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}

#endif // !defined(_WIN32)
