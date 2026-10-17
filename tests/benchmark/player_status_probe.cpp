// Build in an MSVC developer shell:
// cl /nologo /std:c++17 /EHsc player_status_probe.cpp /Fe:player_status_probe.exe user32.lib shell32.lib advapi32.lib
// Queries only the explicitly supplied PID. --stop explicitly closes that PID's
// playback before querying, for the direct-launch/return-to-library regression.
#include "../../src/IpcProtocol.h"
#include <iostream>
#include <iomanip>
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
int main(int argc, char** argv) {
    if (argc != 2 && (argc != 3 || strcmp(argv[2], "--stop") != 0)) return 2;
    char* end = nullptr; const auto parsed = strtoul(argv[1], &end, 10);
    if (!parsed || *end) return 2;
    targetPid = static_cast<DWORD>(parsed);
    EnumWindows(FindOwned, 0);
    if (!target) return 3;
    WNDCLASSW cls{}; cls.lpfnWndProc = Reply; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"Aperture_Benchmark_Status_Reply";
    if (!RegisterClassW(&cls)) return 4;
    HWND reply = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    if (!reply) return 4;
    if (argc == 3) {
        Ipc::AutomationCommand stop; stop.op = Ipc::AutomationOp::Stop;
        if (!Ipc::SendAutomationCommand(target, stop)) { DestroyWindow(reply); return 5; }
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
