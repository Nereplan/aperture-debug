#include "MediaFoundation.h"
#include "Common.h"

namespace Aperture {
namespace {

// POD-only delay gates (no C++ objects with destructors, so __try is legal
// under /EHsc). All locking happens in the callers, never across the call.
HRESULT DelaySafeMFStartup() {
    if (!SystemDllPresent(L"MFPlat.DLL") || !SystemDllPresent(L"MFReadWrite.dll")) { return E_FAIL; }
    HRESULT hr = E_FAIL;
    __try {
        hr = MFStartup(MF_VERSION);
    } __except (DelayLoadExceptionFilter(GetExceptionCode())) { hr = E_FAIL; }
    return hr;
}

void DelaySafeMFShutdown() {
    if (!SystemDllPresent(L"MFPlat.DLL")) { return; }
    __try {
        MFShutdown();
    } __except (DelayLoadExceptionFilter(GetExceptionCode())) { return; }
}

} // namespace

std::mutex& MediaFoundation::Mutex() { static std::mutex mtx; return mtx; }

int& MediaFoundation::RefCount() { static int count = 0; return count; }

bool& MediaFoundation::InitializedFlag() { static bool initialized = false; return initialized; }

bool MediaFoundation::Acquire() {
    {
        std::lock_guard<std::mutex> lock(Mutex());
        if (InitializedFlag()) { ++RefCount(); return true; }
    }
    // MFStartup outside the lock: no mutex is held across the (possibly delay-loaded) system call, so a load failure can never deadlock.
    const HRESULT hr = DelaySafeMFStartup();
    std::lock_guard<std::mutex> lock(Mutex());
    if (SUCCEEDED(hr)) { InitializedFlag() = true; ++RefCount(); return true; }
    return false;
}

void MediaFoundation::Release() {
    bool doShutdown = false;
    {
        std::lock_guard<std::mutex> lock(Mutex());
        if (RefCount() > 0) { --RefCount(); }
        if (RefCount() == 0 && InitializedFlag()) { InitializedFlag() = false; doShutdown = true; }
    }
    if (doShutdown) { DelaySafeMFShutdown(); }
}

bool MediaFoundation::IsInitialized() { std::lock_guard<std::mutex> lock(Mutex()); return InitializedFlag(); }

void MediaFoundation::ShutdownIfInitialized() {
    bool doShutdown = false;
    {
        std::lock_guard<std::mutex> lock(Mutex());
        if (InitializedFlag()) { InitializedFlag() = false; RefCount() = 0; doShutdown = true; }
    }
    if (doShutdown) { DelaySafeMFShutdown(); }
}

} // namespace Aperture
