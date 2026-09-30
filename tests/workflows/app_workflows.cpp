// Exercises the actual executable, mpv, command dispatch and portable storage.
// Uses the public automation protocol; no screen coordinates or app internals.
#include "IpcProtocol.h"
#include "Json.h"
#include "TestMedia.h"
#include <iostream>
#include <functional>
#include <chrono>
#include <thread>
#include <iterator>

using namespace Aperture;
namespace fs = std::filesystem;
static Ipc::AutomationStatus reply;
static bool received;
static HWND replyWindow;
static std::vector<double> latency;
static void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static LRESULT CALLBACK ReplyProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_COPYDATA) {
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(l);
        uint32_t status = 1;
        received = data && data->dwData == Ipc::IPC_MAGIC_V2 &&
            Ipc::DeserializeAutomationResponse(data->lpData, data->cbData, reply, &status, nullptr, true) == Ipc::ValidationResult::Success && status == 0;
        return received;
    }
    return DefWindowProcW(window, message, w, l);
}
struct AppProcess {
    PROCESS_INFORMATION process{};
    HWND window = nullptr;
    ULONGLONG launchStart = 0, windowMs = 0;
    ~AppProcess() {
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
                DWORD owner = 0;
                if (window) GetWindowThreadProcessId(window, &owner);
                if (owner == process.dwProcessId) PostMessageW(window, WM_CLOSE, 0, 0);
                if (WaitForSingleObject(process.hProcess, 5000) == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 5000);
            }
            CloseHandle(process.hProcess); CloseHandle(process.hThread);
        }
    }
    void Launch(const fs::path& exe, const fs::path& media, bool visible = false) {
        Require(!FindWindowW(L"Aperture_Native_Direct2D_Class", nullptr), "An Aperture instance is already open; refusing to touch its session");
        HANDLE existing = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Aperture_SingleInstance_Mutex_V1");
        const DWORD mutexError = GetLastError();
        if (existing) CloseHandle(existing);
        Require(!existing && mutexError == ERROR_FILE_NOT_FOUND, "An Aperture instance is starting or cannot be isolated safely");
        std::wstring command = L"\"" + exe.wstring() + L"\" --paused --volume=0 \"" + media.wstring() + L"\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = visible ? SW_SHOWNOACTIVATE : SW_HIDE;
        launchStart = GetTickCount64();
        Require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                               exe.parent_path().c_str(), &startup, &process) != FALSE, "App launch failed");
        const auto deadline = GetTickCount64() + 15000;
        while (!window && GetTickCount64() < deadline) {
            Require(WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT, "App exited during startup");
            EnumWindows([](HWND candidate, LPARAM context) -> BOOL {
                auto* app = reinterpret_cast<AppProcess*>(context);
                DWORD pid = 0; GetWindowThreadProcessId(candidate, &pid);
                wchar_t name[128]{}; GetClassNameW(candidate, name, 128);
                if (pid == app->process.dwProcessId && wcscmp(name, L"Aperture_Native_Direct2D_Class") == 0) {
                    app->window = candidate; return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(this));
            if (window && visible && !IsWindowVisible(window)) window = nullptr;
            if (!window) Sleep(25);
        }
        Require(window != nullptr, "Owned app window did not appear");
        windowMs = GetTickCount64() - launchStart;
    }
    void Send(Ipc::AutomationCommand command) {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        Require(pid == process.dwProcessId && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT, "App is no longer responsive or owned by test");
        // The wire protocol rejects identical requests in the same clock tick.
        static ULONGLONG lastTick = 0;
        while (GetTickCount64() == lastTick) Sleep(1);
        lastTick = GetTickCount64();
        auto envelope = Ipc::SerializeAutomationEnvelope(command);
        COPYDATASTRUCT data{Ipc::IPC_MAGIC_V2, DWORD(envelope.size()), envelope.data()};
        DWORD_PTR result = 0;
        received = false;
        const auto start = std::chrono::steady_clock::now();
        // Allow incoming synchronous replies while awaiting the target.
        const BOOL sent = SendMessageTimeoutW(window, WM_COPYDATA, reinterpret_cast<WPARAM>(replyWindow),
            reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 3000, &result);
        latency.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        Require(sent && result, "Automation request timed out or was rejected");
        if (command.op == Ipc::AutomationOp::QueryStatus) Require(received, "App acknowledged status without sending a valid response");
    }
    void Command(Ipc::AutomationOp op) { Ipc::AutomationCommand command; command.op = op; Send(command); }
    Ipc::AutomationStatus Status() { Command(Ipc::AutomationOp::QueryStatus); return reply; }
    void ClickBackChevron() {
        HWND overlay = nullptr;
        for (HWND candidate = GetTopWindow(nullptr); candidate; candidate = GetWindow(candidate, GW_HWNDNEXT)) {
            DWORD pid = 0; GetWindowThreadProcessId(candidate, &pid);
            wchar_t name[128]{}; GetClassNameW(candidate, name, 128);
            if (pid == process.dwProcessId && wcscmp(name, L"ApertureOverlay") == 0) { overlay = candidate; break; }
        }
        Require(overlay != nullptr, "Playback overlay not found");
        const UINT dpi = GetDpiForWindow(window);
        const LPARAM point = MAKELPARAM(MulDiv(42, dpi, 96), MulDiv(34, dpi, 96));
        PostMessageW(overlay, WM_MOUSEMOVE, 0, point);
        PostMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, point);
        PostMessageW(overlay, WM_LBUTTONUP, 0, point);
    }
    Ipc::AutomationStatus Until(const char* description, const std::function<bool(const Ipc::AutomationStatus&)>& predicate) {
        const auto start = GetTickCount64();
        do {
            const auto state = Status();
            if (predicate(state)) {
                std::cout << "PASS " << description << " elapsed_ms=" << GetTickCount64() - start
                          << " position=" << state.positionSec << " duration=" << state.durationSec << std::endl;
                return state;
            }
            Sleep(40);
        } while (GetTickCount64() - start < 12000);
        std::cerr << "Last state=" << int(reply.state) << " position=" << reply.positionSec << " duration=" << reply.durationSec << std::endl;
        throw std::runtime_error(description);
    }
    void Close() {
        PostMessageW(window, WM_CLOSE, 0, 0);
        Require(WaitForSingleObject(process.hProcess, 8000) == WAIT_OBJECT_0, "Graceful shutdown timed out");
        DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
        Require(code == 0, "App returned a nonzero exit code");
        window = nullptr;
    }
};

int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 2 || argc == 3, "Expected isolated app executable path and optional benchmark media");
        const fs::path exe = fs::absolute(argv[1]);
        const auto first = exe.parent_path() / L"media" / L"Café first.wav";
        const auto second = exe.parent_path() / L"media" / L"日本語 second.wav";
        if (argc == 2) { WriteWorkflowWave(first, 45); WriteWorkflowWave(second, 60); }
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = ReplyProc; windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"Aperture_Workflow_Reply";
        Require(RegisterClassW(&windowClass) != 0, "Reply class registration failed");
        replyWindow = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, windowClass.hInstance, nullptr);
        Require(replyWindow != nullptr, "Reply window creation failed");
        if (argc == 3) {
            const fs::path media = fs::absolute(argv[2]);
            AppProcess app;
            app.Launch(exe, media, true);
            app.Until("benchmark CLI playable", [](const auto& state) {
                return state.state == Ipc::AutomationPlaybackState::Paused && state.durationSec > 0;
            });
            const auto cliLoadedMs = GetTickCount64() - app.launchStart;
            app.Command(Ipc::AutomationOp::Play);
            app.Until("benchmark clock advances", [](const auto& state) { return state.positionSec > 0.1; });
            app.Command(Ipc::AutomationOp::Stop);
            app.Until("benchmark returns to hub", [](const auto& state) { return state.state == Ipc::AutomationPlaybackState::Closed; });
            Ipc::AutomationCommand open;
            open.op = Ipc::AutomationOp::Open; open.mediaTargets = {media.wstring()}; open.paused = true;
            const auto hubStart = GetTickCount64();
            app.Send(open);
            app.Until("benchmark hub playable", [](const auto& state) {
                return state.state == Ipc::AutomationPlaybackState::Paused && state.durationSec > 0;
            });
            std::cout << "BENCH window_ms=" << app.windowMs << " cli_loaded_ms=" << cliLoadedMs
                << " hub_loaded_ms=" << GetTickCount64() - hubStart << std::endl;
            app.Close();
            DestroyWindow(replyWindow);
            return 0;
        }
        double persistedResume = -1;
        for (int cycle = 0; cycle < 3; ++cycle) {
            AppProcess app;
            app.Launch(exe, first);
            const auto initial = app.Until("load real media paused", [&](const auto& state) {
                return state.state == Ipc::AutomationPlaybackState::Paused && state.currentFilePath == first.wstring() && std::abs(state.durationSec - 45) < 0.2;
            });
            if (persistedResume >= 0) Require(std::abs(initial.positionSec - persistedResume) < 1.0, "Restart did not restore the saved resume position");
            if (cycle == 0) {
                for (bool maximized : {false, true}) {
                    ShowWindow(app.window, maximized ? SW_MAXIMIZE : SW_RESTORE);
                    RECT expected{}; Require(GetWindowRect(app.window, &expected), "Cannot read windowed bounds");
                    for (bool useChevron : {false, true}) {
                        Ipc::AutomationCommand fullscreen;
                        fullscreen.op = Ipc::AutomationOp::Open; fullscreen.mediaTargets = {first.wstring()};
                        fullscreen.paused = true; fullscreen.fullscreen = true;
                        app.Send(fullscreen);
                        app.Until("playback enters fullscreen", [](const auto& state) {
                            return state.isFullscreen && state.state == Ipc::AutomationPlaybackState::Paused;
                        });
                        if (useChevron) app.ClickBackChevron();
                        else PostMessageW(app.window, WM_KEYDOWN, VK_ESCAPE, 1);
                        app.Until(useChevron ? "chevron exits fullscreen" : "Escape exits fullscreen", [](const auto& state) {
                            return !state.isFullscreen && state.state == Ipc::AutomationPlaybackState::Paused;
                        });
                        RECT actual{}; Require(GetWindowRect(app.window, &actual), "Cannot read restored bounds");
                        Require(EqualRect(&expected, &actual), "Fullscreen exit changed the saved window bounds");
                        Require((IsZoomed(app.window) != FALSE) == maximized, "Fullscreen exit changed the saved maximize state");
                    }
                }
            }
            Ipc::AutomationCommand seek; seek.op = Ipc::AutomationOp::Seek; seek.seekPositionSec = 4 + cycle;
            app.Send(seek);
            app.Until("seek reaches requested position", [&](const auto& state) { return std::abs(state.positionSec - seek.seekPositionSec) < 0.2; });
            app.Command(Ipc::AutomationOp::Play);
            app.Until("playback clock advances", [&](const auto& state) { return state.state == Ipc::AutomationPlaybackState::Playing && state.positionSec > seek.seekPositionSec + 0.25; });
            app.Command(Ipc::AutomationOp::Pause);
            const auto paused = app.Until("pause acknowledged", [](const auto& state) { return state.state == Ipc::AutomationPlaybackState::Paused; });
            Sleep(300);
            Require(std::abs(app.Status().positionSec - paused.positionSec) < 0.15, "Playback clock advanced while paused");
            Ipc::AutomationCommand enqueue; enqueue.op = Ipc::AutomationOp::Enqueue; enqueue.mediaTargets = {second.wstring()};
            app.Send(enqueue); app.Command(Ipc::AutomationOp::Next);
            app.Until("next queue item loads", [&](const auto& state) { return state.currentFilePath == second.wstring() && std::abs(state.durationSec - 60) < 0.2; });
            app.Command(Ipc::AutomationOp::Previous);
            app.Until("previous queue item returns", [&](const auto& state) { return state.currentFilePath == first.wstring() && std::abs(state.durationSec - 45) < 0.2; });
            app.Send(seek);
            app.Until("position restored before close", [&](const auto& state) { return std::abs(state.positionSec - seek.seekPositionSec) < 0.3; });
            app.Command(Ipc::AutomationOp::Stop);
            app.Until("stop returns to library", [](const auto& state) { return state.state == Ipc::AutomationPlaybackState::Closed; });
            Ipc::AutomationCommand reopen; reopen.op = Ipc::AutomationOp::Open; reopen.mediaTargets = {first.wstring()}; reopen.paused = true;
            const auto hubOpenStart = GetTickCount64();
            app.Send(reopen);
            app.Until("hub reopens media", [&](const auto& state) {
                return state.state == Ipc::AutomationPlaybackState::Paused && state.currentFilePath == first.wstring() &&
                    std::abs(state.durationSec - 45) < 0.2;
            });
            std::cout << "PASS hub_to_video_ms=" << GetTickCount64() - hubOpenStart << std::endl;
            app.Command(Ipc::AutomationOp::Stop);
            app.Until("reopen returns to library", [](const auto& state) { return state.state == Ipc::AutomationPlaybackState::Closed; });
            app.Close();
            std::ifstream input(exe.parent_path() / L"data" / L"library.json", std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            JsonValue saved;
            Require(JsonValue::parse(text, saved) && saved.is_array(), "App did not persist a readable library");
            bool found = false;
            for (const auto& item : saved.as_array()) {
                if (item.get("filePath").as_wstring().find(first.filename().wstring()) == std::wstring::npos) continue;
                found = true;
                Require(std::abs(item.get("lastPosSec").as_number() - seek.seekPositionSec) < 1.0, "App saved an incorrect resume position");
                persistedResume = item.get("lastPosSec").as_number();
                Require(std::abs(item.get("durationSec").as_number() - 45) < 0.2, "App saved incorrect decoder metadata");
            }
            Require(found, "Played media absent from saved library");
            std::cout << "PASS restart_cycle=" << cycle << " persisted_resume=true" << std::endl;
        }
        std::sort(latency.begin(), latency.end());
        const double p95 = latency[(latency.size() - 1) * 95 / 100];
        std::cout << "PASS command_count=" << latency.size() << " command_p95_ms=" << p95 << std::endl;
        Require(p95 < 1000, "Command responsiveness exceeded 1 second p95 budget");
        DestroyWindow(replyWindow); UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << std::endl;
        return 1;
    }
    return 0;
}
