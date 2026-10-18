// Local, owned-process diagnostic sampler. Timing runs must be performed without
// this sampler attached: suspending threads and walking stacks adds overhead.
// cl /nologo /std:c++17 /EHsc /O2 profile_player.cpp /Fe:profile_player.exe dbghelp.lib
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

struct Handle {
    HANDLE h = nullptr;
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
static uint64_t Ticks(const FILETIME& value) {
    return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
static std::string Utf8(const wchar_t* value) {
    const int length = static_cast<int>(wcslen(value));
    const int n = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
    std::string result(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, length, result.data(), n, nullptr, nullptr);
    return result;
}
static std::string Json(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '\\' || c == '"') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c >= 32) out += c;
    }
    return out + '"';
}
struct ThreadData {
    uint64_t first = 0, last = 0, previous = 0;
    bool seen = false;
    bool cyclesSeen = false;
    uint64_t firstCycles = 0, lastCycles = 0, cycleReadFailures = 0;
    std::string name;
    std::map<std::string, uint64_t> sampledStacks;
};
struct Member { ULONG type = 0; DWORD offset = 0; };
static bool MemberInfo(HANDLE process, DWORD64 module, ULONG type, const wchar_t* name, Member& out) {
    ULONG count = 0;
    if (!SymGetTypeInfo(process, module, type, TI_GET_CHILDRENCOUNT, &count) || !count) return false;
    std::vector<unsigned char> storage(sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG));
    auto* children = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(storage.data());
    children->Count = count; children->Start = 0;
    if (!SymGetTypeInfo(process, module, type, TI_FINDCHILDREN, children)) return false;
    for (ULONG i = 0; i < count; ++i) {
        PWSTR memberName = nullptr;
        if (!SymGetTypeInfo(process, module, children->ChildId[i], TI_GET_SYMNAME, &memberName)) continue;
        const bool matches = memberName && wcscmp(memberName, name) == 0;
        LocalFree(memberName);
        if (matches) return SymGetTypeInfo(process, module, children->ChildId[i], TI_GET_OFFSET, &out.offset) &&
            SymGetTypeInfo(process, module, children->ChildId[i], TI_GET_TYPEID, &out.type);
    }
    return false;
}
template<class T> static bool Read(HANDLE process, DWORD64 address, T& value) {
    SIZE_T bytes = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), &value, sizeof(value), &bytes) && bytes == sizeof(value);
}
struct AppProbe {
    DWORD64 pointer = 0; DWORD hubPresented = 0, opacity = 0, idleTimer = 0, frames = 0, progressTimer = 0;
    DWORD bitmap = 0, presentedFrames = 0, presentationFailures = 0;
    bool Init(HANDLE process) {
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = MAX_SYM_NAME;
        if (!SymFromName(process, "g_pApp", symbol)) return false;
        pointer = symbol->Address;
        ULONG appType = 0;
        if (!SymGetTypeInfo(process, symbol->ModBase, symbol->TypeIndex, TI_GET_TYPEID, &appType)) return false;
        Member renderer{}, presented{}, alpha{}, idle{}, progress{}, overlay{}, stats{}, drawn{}, surface{}, presentedCount{}, failures{};
        if (!MemberInfo(process, symbol->ModBase, appType, L"renderer", renderer) ||
            !MemberInfo(process, symbol->ModBase, renderer.type, L"hasPresentedFrame", presented) ||
            !MemberInfo(process, symbol->ModBase, appType, L"controlsOpacity", alpha) ||
            !MemberInfo(process, symbol->ModBase, appType, L"controlsIdleTimer", idle) ||
            !MemberInfo(process, symbol->ModBase, appType, L"progressSaveTimerSec", progress) ||
            !MemberInfo(process, symbol->ModBase, appType, L"overlayRenderer", overlay) ||
            !MemberInfo(process, symbol->ModBase, overlay.type, L"frameStatistics", stats) ||
            !MemberInfo(process, symbol->ModBase, stats.type, L"framesDrawn", drawn) ||
            !MemberInfo(process, symbol->ModBase, overlay.type, L"hBitmap", surface) ||
            !MemberInfo(process, symbol->ModBase, stats.type, L"framesPresented", presentedCount) ||
            !MemberInfo(process, symbol->ModBase, stats.type, L"presentationFailures", failures)) return false;
        hubPresented = renderer.offset + presented.offset; opacity = alpha.offset; idleTimer = idle.offset;
        frames = overlay.offset + stats.offset + drawn.offset;
        progressTimer = progress.offset;
        bitmap = overlay.offset + surface.offset;
        presentedFrames = overlay.offset + stats.offset + presentedCount.offset;
        presentationFailures = overlay.offset + stats.offset + failures.offset;
        return true;
    }
    std::string Snapshot(HANDLE process, double elapsed) const {
        DWORD64 app = 0; bool shown = false; float alpha = 0, idle = 0, progress = 0; uint64_t frameCount = 0;
        DWORD64 bitmapHandle = 0; uint64_t presentedCount = 0, failures = 0;
        if (!Read(process, pointer, app) || !app || !Read(process, app + hubPresented, shown) ||
            !Read(process, app + opacity, alpha) || !Read(process, app + idleTimer, idle) ||
            !Read(process, app + progressTimer, progress) || !Read(process, app + frames, frameCount) ||
            !Read(process, app + bitmap, bitmapHandle) || !Read(process, app + presentedFrames, presentedCount) ||
            !Read(process, app + presentationFailures, failures)) return "null";
        return "{\"elapsedSeconds\":" + std::to_string(elapsed) + ",\"hubPresented\":" + (shown ? "true" : "false") +
            ",\"controlsOpacity\":" + std::to_string(alpha) + ",\"controlsIdleSeconds\":" + std::to_string(idle) +
            ",\"progressSaveTimerSeconds\":" + std::to_string(progress) +
            ",\"overlayFramesDrawn\":" + std::to_string(frameCount) +
            ",\"overlayBitmapAllocated\":" + (bitmapHandle ? "true" : "false") +
            ",\"overlayFramesPresented\":" + std::to_string(presentedCount) +
            ",\"overlayPresentationFailures\":" + std::to_string(failures) + "}";
    }
};
static std::string Stack(HANDLE process, HANDLE thread) {
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) return "unavailable";
    struct Suspension {
        HANDLE thread;
        ~Suspension() { if (thread) ResumeThread(thread); }
        void Resume() { ResumeThread(thread); thread = nullptr; }
    } suspension{thread};
    CONTEXT context{}; context.ContextFlags = CONTEXT_FULL;
    DWORD64 addresses[18]{};
    int addressCount = 0;
    if (GetThreadContext(thread, &context)) {
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip; frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp; frame.AddrStack.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
        for (int depth = 0; depth < 18 && frame.AddrPC.Offset; ++depth) {
            addresses[addressCount++] = frame.AddrPC.Offset;
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context,
                nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        }
    }
    suspension.Resume();
    // Symbol lookup occurs after resuming the target thread.
    std::string result;
    for (int i = 0; i < addressCount; ++i) {
        const DWORD64 address = addresses[i];
        if (!result.empty()) result += " <- ";
        IMAGEHLP_MODULE64 module{}; module.SizeOfStruct = sizeof(module);
        if (SymGetModuleInfo64(process, address, &module)) result += std::string(module.ModuleName) + "!";
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, address, &displacement, symbol)) result += symbol->Name;
        else result += std::to_string(address - module.BaseOfImage);
        result += " [rva=" + std::to_string(address - module.BaseOfImage) + "]";
    }
    return result.empty() ? "unavailable" : result;
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 5) return 2;
    const DWORD target = wcstoul(argv[1], nullptr, 10);
    const int seconds = _wtoi(argv[2]);
    if (!target || seconds < 1 || seconds > 300) return 2;
    const bool stateOnly = argc == 5 && wcscmp(argv[4], L"--state-only") == 0;
    const bool cpuOnly = argc == 5 && wcscmp(argv[4], L"--cpu-only") == 0;
    if (argc == 5 && !stateOnly && !cpuOnly) return 2;
    Handle process{OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, target)};
    if (!process.h) return 3;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    if (!SymInitializeW(process.h, argc >= 4 ? argv[3] : nullptr, TRUE)) return 4;
    AppProbe appProbe;
    const bool appFieldsAvailable = appProbe.Init(process.h);
    std::vector<std::string> snapshots;
    double nextSnapshot = 0;
    std::map<std::pair<DWORD, uint64_t>, ThreadData> threads;
    const auto started = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - started < std::chrono::seconds(seconds)) {
        if (WaitForSingleObject(process.h, 0) == WAIT_OBJECT_0) break;
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (appFieldsAvailable && elapsed >= nextSnapshot) { snapshots.push_back(appProbe.Snapshot(process.h, elapsed)); nextSnapshot = elapsed + 1.0; }
        if (stateOnly) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
        Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)};
        THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot.h, &entry)) do {
            if (entry.th32OwnerProcessID != target) continue;
            const DWORD access = THREAD_QUERY_INFORMATION | (cpuOnly ? 0 : THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME);
            Handle thread{OpenThread(access, FALSE, entry.th32ThreadID)};
            if (!thread.h) continue;
            FILETIME create{}, exit{}, kernel{}, user{};
            if (!GetThreadTimes(thread.h, &create, &exit, &kernel, &user)) continue;
            auto& record = threads[{entry.th32ThreadID, Ticks(create)}];
            ULONG64 cycles = 0;
            if (QueryThreadCycleTime(thread.h, &cycles)) {
                if (!record.cyclesSeen) { record.cyclesSeen = true; record.firstCycles = cycles; }
                record.lastCycles = cycles;
            } else { ++record.cycleReadFailures; }
            const uint64_t ticks = Ticks(kernel) + Ticks(user);
            if (!record.seen) { record.seen = true; record.first = ticks; record.previous = ticks; }
            if (record.name.empty()) {
                PWSTR name = nullptr;
                if (SUCCEEDED(GetThreadDescription(thread.h, &name)) && name) { record.name = Utf8(name); LocalFree(name); }
            }
            const uint64_t delta = ticks >= record.previous ? ticks - record.previous : 0;
            record.last = ticks;
            if (delta && !cpuOnly) record.sampledStacks[Stack(process.h, thread.h)] += delta;
            record.previous = ticks;
        } while (Thread32Next(snapshot.h, &entry));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    const double observedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    SymCleanup(process.h);
    std::cout << "{\"pid\":" << target << ",\"durationSeconds\":"
        << observedSeconds
        << ",\"method\":\"Per-thread CPU deltas. Stack mode suspends threads after active intervals, not exact attribution. CPU-only and state-only modes do not suspend. Diagnostic runs remain separate from rankings.\",\"stateOnly\":" << (stateOnly ? "true" : "false") << ",\"cpuOnly\":" << (cpuOnly ? "true" : "false")
        << ",\"appFieldsAvailable\":" << (appFieldsAvailable ? "true" : "false") << ",\"snapshots\":[";
    for (size_t i = 0; i < snapshots.size(); ++i) { if (i) std::cout << ','; std::cout << snapshots[i]; }
    std::cout << "],\"threads\":[";
    bool first = true;
    for (const auto& item : threads) {
        if (!first) std::cout << ','; first = false;
        std::cout << "{\"id\":" << item.first.first << ",\"createdTicks\":" << item.first.second << ",\"name\":" << Json(item.second.name)
            << ",\"cpuSeconds\":" << static_cast<double>(item.second.last - item.second.first) / 1e7
            << ",\"cyclesAvailable\":" << (item.second.cyclesSeen ? "true" : "false")
            << ",\"cyclesDelta\":" << (item.second.lastCycles - item.second.firstCycles)
            << ",\"cycleReadFailures\":" << item.second.cycleReadFailures << ",\"stacks\":[";
        std::vector<std::pair<std::string, uint64_t>> stacks(item.second.sampledStacks.begin(), item.second.sampledStacks.end());
        std::sort(stacks.begin(), stacks.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < std::min<size_t>(stacks.size(), 8); ++i) {
            if (i) std::cout << ',';
            std::cout << "{\"stack\":" << Json(stacks[i].first) << ",\"precedingCpuSeconds\":" << static_cast<double>(stacks[i].second) / 1e7 << '}';
        }
        std::cout << "]}";
    }
    std::cout << "]}\n";
    return 0;
}
