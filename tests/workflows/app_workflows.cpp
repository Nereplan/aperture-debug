// Exercises the actual executable, mpv, command dispatch and portable storage.
// Uses the public automation protocol; no screen coordinates or app internals.
#include "IpcProtocol.h"
#include "Json.h"
#include "Settings.h"
#include "MediaLibrary.h"
#include "TestMedia.h"
#include <iostream>
#include <functional>
#include <chrono>
#include <thread>
#include <iterator>
#include <uiautomation.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "dbghelp.lib")

using namespace Aperture;
namespace fs = std::filesystem;
static Ipc::AutomationStatus reply;
static bool received;
static HWND replyWindow;
static std::vector<double> latency;
static void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static void PrintOwnedUiStack(const PROCESS_INFORMATION& process) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
    DWORD exitCode = 0; GetExitCodeProcess(process.hProcess, &exitCode);
    std::cerr << "Owned process exit=" << exitCode << std::endl;
    if (!SymInitialize(process.hProcess, nullptr, TRUE)) { std::cerr << "Symbol init error=" << GetLastError() << std::endl; return; }
    HANDLE threads = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    if (threads != INVALID_HANDLE_VALUE && Thread32First(threads, &entry)) do {
        if (entry.th32OwnerProcessID != process.dwProcessId) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        std::cerr << "THREAD " << entry.th32ThreadID << (entry.th32ThreadID == process.dwThreadId ? " UI" : "") << std::endl;
        if (SuspendThread(thread) != DWORD(-1)) {
            CONTEXT context{}; context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                STACKFRAME64 frame{};
                frame.AddrPC = { context.Rip, 0, AddrModeFlat };
                frame.AddrFrame = { context.Rbp, 0, AddrModeFlat };
                frame.AddrStack = { context.Rsp, 0, AddrModeFlat };
                for (unsigned i = 0; i < 20 && frame.AddrPC.Offset; ++i) {
                    const DWORD64 base = SymGetModuleBase64(process.hProcess, frame.AddrPC.Offset);
                    IMAGEHLP_MODULE64 module{}; module.SizeOfStruct = sizeof(module); SymGetModuleInfo64(process.hProcess, frame.AddrPC.Offset, &module);
                    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
                    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
                    symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = MAX_SYM_NAME;
                    DWORD64 displacement = 0;
                    std::cerr << "stack " << i << " " << module.ModuleName << " rva=0x" << std::hex << (frame.AddrPC.Offset-base) << std::dec << " ";
                    if (SymFromAddr(process.hProcess, frame.AddrPC.Offset, &displacement, symbol)) std::cerr << symbol->Name;
                    std::cerr << std::endl;
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process.hProcess, thread, &frame, &context,
                        nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
                }
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    } while (Thread32Next(threads, &entry));
    if (threads != INVALID_HANDLE_VALUE) CloseHandle(threads);
    SymCleanup(process.hProcess);
}
static JsonValue ReadStore(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(input.good(), "Expected portable store does not exist");
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    JsonValue result;
    Require(JsonValue::parse(text, result), "Portable store is not valid JSON");
    return result;
}
static JsonValue ReadLibraryRecords(const fs::path& path) {
    const auto document = ReadStore(path);
    Require(document.is_object() && document.get("items").is_array() &&
        document.get("schemaVersion").as_number(-1) == MediaLibrary::kCurrentLibrarySchemaVersion,
        "App did not persist the current library document");
    return document.get("items");
}
static void WriteStore(const fs::path& path, const JsonValue& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value.stringify(true);
    output.close();
    Require(output.good(), "Could not seed isolated portable store");
}
static std::wstring UiaProperty(IUIAutomationElement* element, PROPERTYID property) {
    VARIANT value{};
    Require(SUCCEEDED(element->GetCurrentPropertyValue(property, &value)), "UIA property request failed");
    std::wstring text = value.vt == VT_BSTR && value.bstrVal ? value.bstrVal : L"";
    VariantClear(&value);
    return text;
}
static Microsoft::WRL::ComPtr<IUIAutomationElement> UiaRoot(HWND window, DWORD ownedPid) {
    Microsoft::WRL::ComPtr<IUIAutomation> automation;
    Require(SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IUIAutomation), reinterpret_cast<void**>(automation.GetAddressOf()))), "UIA client activation failed");
    Microsoft::WRL::ComPtr<IUIAutomationElement> root;
    Require(SUCCEEDED(automation->ElementFromHandle(window, root.GetAddressOf())) && root, "Actual app UIA root was unavailable");
    int pid = 0;
    Require(SUCCEEDED(root->get_CurrentProcessId(&pid)) && static_cast<DWORD>(pid) == ownedPid, "UIA returned another process's root");
    Require(UiaProperty(root.Get(), UIA_AutomationIdPropertyId) == L"ApertureMainWindow", "WM_GETOBJECT did not return the custom UIA root provider");
    Require(UiaProperty(root.Get(), UIA_NamePropertyId) == L"Aperture", "UIA root did not expose the native application name");
    return root;
}
static Microsoft::WRL::ComPtr<IUIAutomationElement> UiaFind(HWND window, DWORD pid, const wchar_t* automationId) {
    Microsoft::WRL::ComPtr<IUIAutomation> automation;
    Require(SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IUIAutomation), reinterpret_cast<void**>(automation.GetAddressOf()))), "UIA client activation failed");
    VARIANT id{}; id.vt = VT_BSTR; id.bstrVal = SysAllocString(automationId);
    Microsoft::WRL::ComPtr<IUIAutomationCondition> condition;
    const HRESULT created = automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, condition.GetAddressOf());
    VariantClear(&id);
    Require(SUCCEEDED(created), "UIA automation-ID condition failed");
    const ULONGLONG start = GetTickCount64();
    do {
        auto root = UiaRoot(window, pid);
        Microsoft::WRL::ComPtr<IUIAutomationElement> element;
        Require(SUCCEEDED(root->FindFirst(TreeScope_Descendants, condition.Get(), element.GetAddressOf())), "UIA descendant lookup failed");
        if (element) return element;
        Sleep(25);
    } while (GetTickCount64() - start < 3000);
    throw std::runtime_error("Expected UIA surface did not appear");
}
template<class Pattern> static Microsoft::WRL::ComPtr<Pattern> UiaPattern(IUIAutomationElement* element, PATTERNID id) {
    Microsoft::WRL::ComPtr<Pattern> pattern;
    Require(SUCCEEDED(element->GetCurrentPatternAs(id, __uuidof(Pattern), reinterpret_cast<void**>(pattern.GetAddressOf()))) && pattern,
        "Actual app did not expose the expected UIA pattern");
    return pattern;
}
static std::wstring UiaValue(IUIAutomationValuePattern* value) {
    BSTR text = nullptr;
    Require(SUCCEEDED(value->get_CurrentValue(&text)), "UIA Value read failed");
    const std::wstring result = text ? text : L"";
    SysFreeString(text);
    return result;
}
static HRESULT SetUiaValue(IUIAutomationValuePattern* value, const std::wstring& text) {
    struct BstrScope {
        BSTR text;
        ~BstrScope() { SysFreeString(text); }
    } allocated{SysAllocStringLen(text.data(), static_cast<UINT>(text.size()))};
    Require(allocated.text != nullptr, "UIA Value text allocation failed");
    return value->SetValue(allocated.text);
}
static void CheckHubUiaPatterns(HWND window, DWORD pid) {
    auto search = UiaFind(window, pid, L"MediaToolbar_SearchInput");
    auto value = UiaPattern<IUIAutomationValuePattern>(search.Get(), UIA_ValuePatternId);
    BOOL readOnly = TRUE;
    Require(SUCCEEDED(value->get_CurrentIsReadOnly(&readOnly)) && !readOnly, "Media search Value pattern was read-only");
    const auto originalQuery = UiaValue(value.Get());
    Require(SUCCEEDED(SetUiaValue(value.Get(), L"first")) && UiaValue(value.Get()) == L"first", "UIA Value did not set/read the media search");
    Require(SUCCEEDED(SetUiaValue(value.Get(), originalQuery)) && UiaValue(value.Get()) == originalQuery, "UIA Value did not restore the media search");

    auto mediaTab = UiaFind(window, pid, L"NavTab_0");
    auto selection = UiaPattern<IUIAutomationSelectionPattern>(mediaTab.Get(), UIA_SelectionPatternId);
    BOOL multiple = FALSE;
    Require(SUCCEEDED(selection->get_CurrentCanSelectMultiple(&multiple)) && multiple, "Media selection did not advertise multiple selection");
    Microsoft::WRL::ComPtr<IUIAutomationElementArray> selected;
    Require(SUCCEEDED(selection->GetCurrentSelection(selected.GetAddressOf())), "UIA Selection array read failed");
    int selectedCount = 0;
    Require(selected && SUCCEEDED(selected->get_Length(&selectedCount)), "UIA Selection array was invalid");
    // Navigation tabs support SelectionItem independently of the media list's
    // Selection container. Switch to Settings and restore the active media tab.
    auto settingsTab = UiaFind(window, pid, L"NavTab_3");
    auto settingsSelect = UiaPattern<IUIAutomationSelectionItemPattern>(settingsTab.Get(), UIA_SelectionItemPatternId);
    const HRESULT settingsResult = settingsSelect->Select();
    if (FAILED(settingsResult)) std::cerr << "Settings SelectionItem HRESULT=0x" << std::hex << static_cast<unsigned long>(settingsResult) << std::dec << std::endl;
    Require(SUCCEEDED(settingsResult), "UIA SelectionItem could not open Settings");
    const wchar_t* categories[] = { L"SettingsCategory_Appearance", L"SettingsCategory_Playback", L"SettingsCategory_Audio & Subtitles",
        L"SettingsCategory_Shortcuts", L"SettingsCategory_System" };
    std::wstring originalCategory;
    for (const auto* id : categories) {
        auto category = UiaFind(window, pid, id);
        auto categorySelection = UiaPattern<IUIAutomationSelectionItemPattern>(category.Get(), UIA_SelectionItemPatternId);
        BOOL active = FALSE;
        Require(SUCCEEDED(categorySelection->get_CurrentIsSelected(&active)), "UIA settings-category selection read failed");
        if (active) originalCategory = id;
    }
    Require(!originalCategory.empty(), "Settings did not expose its selected category");
    auto shortcuts = UiaFind(window, pid, L"SettingsCategory_Shortcuts");
    auto shortcutsSelect = UiaPattern<IUIAutomationSelectionItemPattern>(shortcuts.Get(), UIA_SelectionItemPatternId);
    Require(SUCCEEDED(shortcutsSelect->Select()), "UIA SelectionItem could not open Shortcuts");
    // Scroll is conditional on viewport/content geometry. The Shortcuts page
    // offers the longest ordinary settings surface without creating fixtures.
    Microsoft::WRL::ComPtr<IUIAutomationScrollPattern> scroll;
    const ULONGLONG scrollStart = GetTickCount64();
    do {
        shortcuts = UiaFind(window, pid, L"SettingsCategory_Shortcuts");
        VARIANT available{};
        Require(SUCCEEDED(shortcuts->GetCurrentPropertyValue(UIA_IsScrollPatternAvailablePropertyId, &available)) && available.vt == VT_BOOL,
            "UIA Scroll availability could not be read");
        const bool exposed = available.boolVal != VARIANT_FALSE;
        VariantClear(&available);
        if (exposed) scroll = UiaPattern<IUIAutomationScrollPattern>(shortcuts.Get(), UIA_ScrollPatternId);
        if (scroll) break;
        Sleep(25);
    } while (GetTickCount64() - scrollStart < 500);
    bool testedScroll = false;
    if (scroll) {
        BOOL vertical = FALSE;
        double originalPercent = 0.0, viewSize = 0.0;
        Require(SUCCEEDED(scroll->get_CurrentVerticallyScrollable(&vertical)) && vertical, "UIA Scroll did not expose vertical scrolling");
        Require(SUCCEEDED(scroll->get_CurrentVerticalScrollPercent(&originalPercent)) && originalPercent >= 0 && originalPercent <= 100,
            "UIA Scroll percentage was invalid");
        Require(SUCCEEDED(scroll->get_CurrentVerticalViewSize(&viewSize)) && viewSize > 0 && viewSize < 100, "UIA Scroll view size was invalid");
        const double target = originalPercent < 50.0 ? 75.0 : 25.0;
        Require(SUCCEEDED(scroll->SetScrollPercent(UIA_ScrollPatternNoScroll, target)), "UIA Scroll did not accept a valid percentage");
        double actual = originalPercent;
        const ULONGLONG start = GetTickCount64();
        do {
            Require(SUCCEEDED(scroll->get_CurrentVerticalScrollPercent(&actual)), "UIA Scroll percentage read failed after invocation");
            if (std::abs(actual - target) < 1.0) break;
            Sleep(25);
        } while (GetTickCount64() - start < 3000);
        Require(std::abs(actual - target) < 1.0, "UIA Scroll invocation did not update the actual surface");
        Require(SUCCEEDED(scroll->SetScrollPercent(UIA_ScrollPatternNoScroll, originalPercent)), "UIA Scroll did not restore its original percentage");
        testedScroll = true;
    } else std::cout << "SKIP actual_app_uia_scroll=viewport_has_no_overflow" << std::endl;
    auto original = UiaFind(window, pid, originalCategory.c_str());
    Require(SUCCEEDED(UiaPattern<IUIAutomationSelectionItemPattern>(original.Get(), UIA_SelectionItemPatternId)->Select()),
        "UIA SelectionItem did not restore the settings category");
    mediaTab = UiaFind(window, pid, L"NavTab_0");
    Require(SUCCEEDED(UiaPattern<IUIAutomationSelectionItemPattern>(mediaTab.Get(), UIA_SelectionItemPatternId)->Select()),
        "UIA SelectionItem did not restore the media tab");
    UiaFind(window, pid, L"MediaToolbar_SearchInput");
    std::cout << "PASS actual_app_uia_value=true selection=true selection_count=" << selectedCount << " scroll=" << testedScroll << std::endl;
}
static void RequireAnnouncement(HWND window, DWORD pid, const wchar_t* expected) {
    Microsoft::WRL::ComPtr<IUIAutomation> automation;
    Require(SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IUIAutomation), reinterpret_cast<void**>(automation.GetAddressOf()))), "UIA client activation failed");
    const ULONGLONG start = GetTickCount64();
    do {
        auto root = UiaRoot(window, pid);
        VARIANT id{}; id.vt = VT_BSTR; id.bstrVal = SysAllocString(L"AppStatusLiveRegion");
        Microsoft::WRL::ComPtr<IUIAutomationCondition> condition;
        const HRESULT created = automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, condition.GetAddressOf());
        VariantClear(&id);
        Require(SUCCEEDED(created), "UIA live-region condition failed");
        Microsoft::WRL::ComPtr<IUIAutomationElement> status;
        Require(SUCCEEDED(root->FindFirst(TreeScope_Descendants, condition.Get(), status.GetAddressOf())), "UIA live-region query failed");
        if (status && UiaProperty(status.Get(), UIA_NamePropertyId) == expected) return;
        Sleep(25);
    } while (GetTickCount64() - start < 3000);
    throw std::runtime_error("Private-mode shortcut did not update the actual UIA status announcement");
}
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
    void Launch(const fs::path& exe, const fs::path& media, bool visible = false, const std::wstring& options = L"--volume=0") {
        Require(!FindWindowW(L"Aperture_Native_Direct2D_Class", nullptr), "An Aperture instance is already open; refusing to touch its session");
        HANDLE existing = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Aperture_SingleInstance_Mutex_V1");
        const DWORD mutexError = GetLastError();
        if (existing) CloseHandle(existing);
        Require(!existing && mutexError == ERROR_FILE_NOT_FOUND, "An Aperture instance is starting or cannot be isolated safely");
        std::wstring command = L"\"" + exe.wstring() + L"\" --paused " + options;
        if (!media.empty()) command += L" \"" + media.wstring() + L"\"";
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
        const ULONGLONG readyDeadline = GetTickCount64() + 10000;
        bool ready = false;
        while (!ready && GetTickCount64() < readyDeadline) {
            HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, L"Local\\Aperture_WindowReady_V1");
            if (event) { ready = WaitForSingleObject(event, 50) == WAIT_OBJECT_0; CloseHandle(event); }
            else Sleep(25);
            Require(WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT, "App exited before readiness");
        }
        Require(ready, "Owned app did not signal IPC readiness");
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
        if (!sent || !result) {
            std::cerr << "IPC op=" << int(command.op) << " sent=" << sent << " result=" << result
                << " error=" << GetLastError() << " pid=" << pid << " latency_ms=" << latency.back() << std::endl;
            PrintOwnedUiStack(process);
            throw std::runtime_error("Automation request timed out or was rejected");
        }
        if (command.op == Ipc::AutomationOp::QueryStatus) Require(received, "App acknowledged status without sending a valid response");
    }
    void Command(Ipc::AutomationOp op) { Ipc::AutomationCommand command; command.op = op; Send(command); }
    Ipc::AutomationStatus Status() { Command(Ipc::AutomationOp::QueryStatus); return reply; }
    void ForwardVolume(const fs::path& exe, int percent) {
        DWORD owner = 0; GetWindowThreadProcessId(window, &owner);
        Require(owner == process.dwProcessId && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT,
            "CLI forwarding requires the isolated owned primary process");
        PROCESS_INFORMATION secondary{};
        struct SecondaryScope {
            PROCESS_INFORMATION& process;
            ~SecondaryScope() {
                if (!process.hProcess) return;
                if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
                    TerminateProcess(process.hProcess, 1); // Only this fixture's owned secondary.
                    WaitForSingleObject(process.hProcess, 1000);
                }
                CloseHandle(process.hProcess); CloseHandle(process.hThread);
            }
        } secondaryScope{secondary};
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        std::wstring command = L"\"" + exe.wstring() + L"\" --volume=" + std::to_wstring(percent);
        Require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
            exe.parent_path().c_str(), &startup, &secondary) != FALSE, "Owned second-instance CLI launch failed");
        Require(WaitForSingleObject(secondary.hProcess, 15000) == WAIT_OBJECT_0, "Second-instance CLI forwarding did not exit within 15 seconds");
        DWORD exitCode = 1;
        Require(GetExitCodeProcess(secondary.hProcess, &exitCode) && exitCode == 0, "Second-instance CLI forwarding returned an error");
        Until("second-instance CLI volume reaches the primary", [percent](const auto& state) {
            return state.state == Ipc::AutomationPlaybackState::Paused && std::abs(state.volume - percent) < 0.1;
        });
        std::cout << "PASS second_instance_cli_forwarding=true secondary_exit=0 primary_volume=" << percent << std::endl;
    }
    void Key(UINT key) {
        DWORD owner = 0; GetWindowThreadProcessId(window, &owner);
        Require(owner == process.dwProcessId, "Refusing keyboard messages to another process");
        DWORD_PTR ignored = 0;
        Require(SendMessageTimeoutW(window, WM_KEYDOWN, key, 1, SMTO_ABORTIFHUNG, 3000, &ignored) != FALSE,
            "Owned app did not process the shortcut");
        SendMessageTimeoutW(window, WM_KEYUP, key, 1LL << 31, SMTO_ABORTIFHUNG, 3000, &ignored);
    }
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
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComScope { bool initialized; ~ComScope() { if (initialized) CoUninitialize(); } } comScope{SUCCEEDED(com)};
    try {
        Require(argc == 2 || argc == 3, "Expected isolated app executable path and optional benchmark media");
        const fs::path exe = fs::absolute(argv[1]);
        Require(exe.parent_path().filename() == L"app" && exe.parent_path().parent_path().filename().wstring().find(L"Aperture-workflows-") == 0,
            "Native app workflows require the runner's isolated temporary profile");
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
        const auto data = exe.parent_path() / L"data";
        JsonValue settings = JsonValue::Object();
        settings["schemaVersion"] = Settings::kCurrentSettingsSchemaVersion;
        settings["autoUpdateCheckEnabled"] = false;
        settings["hasWindowPlacement"] = true; settings["windowX"] = -25; settings["windowY"] = 100;
        settings["windowWidth"] = 960; settings["windowHeight"] = 600;
        JsonValue shortcut = JsonValue::Object();
        shortcut["vk"] = static_cast<int>(VK_F12);
        settings["shortcuts"]["togglePrivateMode"] = shortcut;
        WriteStore(data / L"settings.json", settings);
        JsonValue seeded = JsonValue::Array();
        for (const auto& path : {second, first}) {
            JsonValue item = JsonValue::Object();
            item["id"] = path == first ? "workflow-first" : "workflow-second";
            item["filePath"] = path.wstring();
            item["title"] = path.stem().wstring();
            item["durationSec"] = path == first ? 45.0 : 60.0;
            item["preferences"]["playbackSpeed"] = path == first ? 0.5 : 2.0;
            seeded.push_back(item);
        }
        WriteStore(data / L"library.json", seeded);
        for (int cycle = 0; cycle < 3; ++cycle) {
            AppProcess app;
            app.Launch(exe, first);
            const auto initial = app.Until("load real media paused", [&](const auto& state) {
                return state.state == Ipc::AutomationPlaybackState::Paused && state.currentFilePath == first.wstring() && std::abs(state.durationSec - 45) < 0.2;
            });
            if (persistedResume >= 0) Require(std::abs(initial.positionSec - persistedResume) < 1.0, "Restart did not restore the saved resume position");
            if (cycle == 0) {
                UiaRoot(app.window, app.process.dwProcessId);
                RECT placement{}; Require(GetWindowRect(app.window, &placement) && placement.left == -25,
                    "Native restore discarded an explicitly valid negative X coordinate");
                std::cout << "PASS native_negative_x_restore=true" << std::endl;
                std::cout << "PASS actual_app_custom_uia_root=true" << std::endl;
                app.Command(Ipc::AutomationOp::Play);
                app.Until("per-media speed starts playback", [](const auto& state) { return state.state == Ipc::AutomationPlaybackState::Playing && state.positionSec > 0.1; });
                const auto startPosition = app.Status().positionSec;
                const auto speedStart = std::chrono::steady_clock::now();
                Sleep(1000);
                const auto endPosition = app.Status().positionSec;
                const double speedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - speedStart).count();
                const double actualSpeed = (endPosition - startPosition) / speedSeconds;
                Require(actualSpeed > 0.3 && actualSpeed < 0.75, "Promotion applied another media item's preferences or ignored the target speed");
                std::cout << "PASS promoted_media_preference_speed=" << actualSpeed << " expected=0.5 other_media=2" << std::endl;
                app.Command(Ipc::AutomationOp::Pause);
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
            if (cycle == 0) CheckHubUiaPatterns(app.window, app.process.dwProcessId);
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
            const auto saved = ReadLibraryRecords(data / L"library.json");
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
        {
            AppProcess app;
            const auto missingSubtitle = exe.parent_path() / L"media" / L"missing-subtitle.srt";
            app.Launch(exe, first, false, L"--volume=25 --subtitle=\"" + missingSubtitle.wstring() + L"\"");
            const auto loaded = app.Until("CLI volume uses percent and missing subtitle is recoverable", [&](const auto& state) {
                return state.state == Ipc::AutomationPlaybackState::Paused && state.currentFilePath == first.wstring() &&
                    state.durationSec > 0 && std::abs(state.volume - 25.0) < 0.1;
            });
            auto volumeElement = UiaFind(app.window, app.process.dwProcessId, L"VolumeSlider");
            auto range = UiaPattern<IUIAutomationRangeValuePattern>(volumeElement.Get(), UIA_RangeValuePatternId);
            double minVolume = -1, maxVolume = -1, initialVolume = -1;
            BOOL rangeReadOnly = TRUE;
            Require(SUCCEEDED(range->get_CurrentMinimum(&minVolume)) && minVolume == 0.0 &&
                SUCCEEDED(range->get_CurrentMaximum(&maxVolume)) && maxVolume == 100.0 &&
                SUCCEEDED(range->get_CurrentIsReadOnly(&rangeReadOnly)) && !rangeReadOnly &&
                SUCCEEDED(range->get_CurrentValue(&initialVolume)) && std::abs(initialVolume - 25.0) < 0.1,
                "Actual volume RangeValue bounds/current value were incorrect");
            Require(SUCCEEDED(range->SetValue(37.0)), "UIA RangeValue volume invocation failed");
            app.Until("UIA RangeValue changes actual player volume", [](const auto& state) { return std::abs(state.volume - 37.0) < 0.1; });
            Require(SUCCEEDED(range->SetValue(initialVolume)), "UIA RangeValue could not restore volume");
            app.Until("UIA RangeValue restores prior volume", [](const auto& state) { return std::abs(state.volume - 25.0) < 0.1; });
            std::cout << "PASS actual_app_uia_range_value=true" << std::endl;
            app.ForwardVolume(exe, 35);
            app.Command(Ipc::AutomationOp::Play);
            app.Until("playback continues after invalid external subtitle", [&](const auto& state) {
                return state.state == Ipc::AutomationPlaybackState::Playing && state.positionSec > loaded.positionSec + 0.25;
            });
            app.Command(Ipc::AutomationOp::Pause);
            Ipc::AutomationCommand volume; volume.op = Ipc::AutomationOp::Volume; volume.volume = 50.0;
            app.Send(volume);
            app.Until("automation volume uses percent", [](const auto& state) { return std::abs(state.volume - 50.0) < 0.1; });
            app.Close();
            Require(std::abs(ReadStore(data / L"settings.json").get("volume").as_number() - 0.5) < 0.001,
                "App did not persist volume in the normalized Settings range");
            AppProcess reload;
            reload.Launch(exe, first, false, L"");
            reload.Until("volume survives a restart without CLI override", [](const auto& state) {
                return state.state == Ipc::AutomationPlaybackState::Paused && state.durationSec > 0 && std::abs(state.volume - 50.0) < 0.1;
            });
            reload.Close();
            std::cout << "PASS volume_cli_automation_save_reload=true bad_subtitle_recoverable=true" << std::endl;
        }
        {
            const auto privateTarget = exe.parent_path() / L"media" / L"private-only.wav";
            WriteWorkflowWave(privateTarget, 30);
            AppProcess app;
            // Private-session shortcut is a playback action in the existing UI.
            // Establish its public baseline before enabling that mode.
            app.Launch(exe, first, false, L"");
            app.Until("private fixture starts with paused existing media", [&](const auto& state) {
                return state.state == Ipc::AutomationPlaybackState::Paused && state.currentFilePath == first.wstring() && state.durationSec > 0;
            });
            Sleep(300); app.Status();
            const auto baselineLibrary = ReadLibraryRecords(data / L"library.json");
            const auto baselineHistory = ReadStore(data / L"history.json");
            const auto baselineSession = ReadStore(data / L"session.json");
            UiaRoot(app.window, app.process.dwProcessId);
            app.Key(VK_F12);
            RequireAnnouncement(app.window, app.process.dwProcessId, L"Private session enabled");
            for (const auto& target : {first, privateTarget}) {
                Ipc::AutomationCommand open; open.op = Ipc::AutomationOp::Open; open.mediaTargets = {target.wstring()}; open.paused = true;
                app.Send(open);
                app.Until("private media opens", [&](const auto& state) {
                    return state.state == Ipc::AutomationPlaybackState::Paused && state.currentFilePath == target.wstring() && state.durationSec > 0;
                });
                Ipc::AutomationCommand seek; seek.op = Ipc::AutomationOp::Seek; seek.seekPositionSec = target == first ? 23.0 : 7.0;
                app.Send(seek);
                app.Until("private seek changes only live playback", [&](const auto& state) { return std::abs(state.positionSec - seek.seekPositionSec) < 0.2; });
                app.Command(Ipc::AutomationOp::Play);
                app.Until("private playback advances", [&](const auto& state) {
                    return state.state == Ipc::AutomationPlaybackState::Playing && state.positionSec > seek.seekPositionSec + 0.25;
                });
                if (target == first) {
                    app.Command(Ipc::AutomationOp::Stop);
                    app.Until("private close returns to hub", [](const auto& state) { return state.state == Ipc::AutomationPlaybackState::Closed; });
                }
            }
            // Exit while the second private target remains open to exercise
            // Shutdown independently of ClosePlayer's progress guards.
            app.Close();
            const auto afterLibrary = ReadLibraryRecords(data / L"library.json");
            auto findFirst = [&](const JsonValue& library) -> const JsonValue& {
                for (const auto& item : library.as_array()) if (item.get("id").as_string() == "workflow-first") return item;
                throw std::runtime_error("Private fixture lost its existing library item");
            };
            const auto& before = findFirst(baselineLibrary);
            const auto& after = findFirst(afterLibrary);
            for (const auto* field : {"lastPosSec", "lastWatchedTimestamp", "status", "preferences"}) {
                Require(before.get(field).stringify(false) == after.get(field).stringify(false), "Private playback changed durable progress, watched state, or preferences");
            }
            for (const auto& item : afterLibrary.as_array()) {
                Require(item.get("filePath").as_wstring().find(privateTarget.filename().wstring()) == std::wstring::npos,
                    "Private playback added its new target to the durable library");
            }
            Require(baselineHistory.stringify(false) == ReadStore(data / L"history.json").stringify(false), "Private playback changed history");
            Require(baselineSession.stringify(false) == ReadStore(data / L"session.json").stringify(false), "Private playback changed the session store");
            std::cout << "PASS private_play_close_shutdown_persistence=true new_private_target_absent=true" << std::endl;
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
