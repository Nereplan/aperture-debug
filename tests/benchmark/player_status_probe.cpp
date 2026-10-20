// Build in an MSVC developer shell:
// cl /nologo /std:c++17 /EHsc player_status_probe.cpp /Fe:player_status_probe.exe user32.lib shell32.lib advapi32.lib
// Queries/controls only the explicitly supplied PID. Optional --stop, --pause
// --play, --open <file> and --seek <seconds> support lifecycle checks.
#include "../../src/IpcProtocol.h"
#include <iostream>
#include <iomanip>
#include <cwchar>
#include <cmath>
#include <cerrno>
using namespace Aperture;
static DWORD targetPid;
static HWND target;
static Ipc::AutomationStatus status;
static bool received;
static BOOL CALLBACK FindOwned(HWND window, LPARAM) {
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    wchar_t name[128]{}; GetClassNameW(window, name, 128);
    if (pid == targetPid && wcscmp(name, L"Aperture_Native_Direct2D_Class") == 0) {
        target = window; return FALSE;
    }
    return TRUE;
}
static LRESULT CALLBACK Reply(HWND window, UINT message, WPARAM sender, LPARAM payload) {
    if (message == WM_COPYDATA) {
        DWORD pid = 0; GetWindowThreadProcessId(reinterpret_cast<HWND>(sender), &pid);
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(payload);
        uint32_t result = 1;
        received = pid == targetPid && data && data->dwData == Ipc::IPC_MAGIC_V2 &&
            Ipc::DeserializeAutomationResponse(data->lpData, data->cbData, status, &result, nullptr, true) == Ipc::ValidationResult::Success && result == 0;
        return received;
    }
    return DefWindowProcW(window, message, sender, payload);
}
int wmain(int argc, wchar_t** argv) {
    const bool control = argc == 3 && (wcscmp(argv[2], L"--stop") == 0 ||
        wcscmp(argv[2], L"--pause") == 0 || wcscmp(argv[2], L"--play") == 0);
    const bool open = argc == 4 && wcscmp(argv[2], L"--open") == 0;
    const bool seek = argc == 4 && wcscmp(argv[2], L"--seek") == 0;
    if (argc != 2 && !control && !open && !seek) return 2;
    double seekSeconds = 0;
    if (seek) {
        wchar_t* end = nullptr;
        errno = 0;
        seekSeconds = wcstod(argv[3], &end);
        if (errno || end == argv[3] || *end || !std::isfinite(seekSeconds) || seekSeconds < 0 || seekSeconds > 86400) return 2;
    }
    wchar_t* end = nullptr; const auto parsed = wcstoul(argv[1], &end, 10);
    if (!parsed || *end) return 2;
    targetPid = static_cast<DWORD>(parsed);
    EnumWindows(FindOwned, 0);
    if (!target) return 3;
    WNDCLASSW cls{}; cls.lpfnWndProc = Reply; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"Aperture_Benchmark_Status_Reply";
    if (!RegisterClassW(&cls)) return 4;
    HWND reply = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    if (!reply) return 4;
    if (control || open || seek) {
        Ipc::AutomationCommand action;
        action.op = open ? Ipc::AutomationOp::Open : seek ? Ipc::AutomationOp::Seek : wcscmp(argv[2], L"--stop") == 0 ? Ipc::AutomationOp::Stop :
                    wcscmp(argv[2], L"--pause") == 0 ? Ipc::AutomationOp::Pause : Ipc::AutomationOp::Play;
        if (open) action.mediaTargets.emplace_back(argv[3]);
        if (seek) { action.seekPositionSec = seekSeconds; action.seekRelative = false; }
        if (!Ipc::SendAutomationCommand(target, action)) { DestroyWindow(reply); return 5; }
    }
    Ipc::AutomationCommand command; command.op = Ipc::AutomationOp::QueryStatus;
    auto envelope = Ipc::SerializeAutomationEnvelope(command);
    COPYDATASTRUCT data{Ipc::IPC_MAGIC_V2, static_cast<DWORD>(envelope.size()), envelope.data()};
    DWORD_PTR result = 0;
    const BOOL sent = SendMessageTimeoutW(target, WM_COPYDATA, reinterpret_cast<WPARAM>(reply), reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 3000, &result);
    DestroyWindow(reply);
    if (!sent || !result || !received) return 5;
    std::cout << std::setprecision(12) << "{\"state\":" << static_cast<unsigned>(status.state)
              << ",\"positionSec\":" << status.positionSec << ",\"durationSec\":" << status.durationSec << "}\n";
    return 0;
}
