#include "MediaFoundation.h"
#include "Common.h"

namespace Aperture {
namespace {

// POD-only delay gates (no C++ objects with destructors, so __try is legal
// under /EHsc). The callers serialize the complete lifecycle transition.
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
MediaFoundation::LifecycleStats& MediaFoundation::Stats() { static LifecycleStats stats; return stats; }
MediaFoundation::LifecycleStats MediaFoundation::GetLifecycleStats() {
    std::lock_guard<std::mutex> lock(Mutex());
    auto stats = Stats(); stats.references = RefCount(); return stats;
}

bool MediaFoundation::Acquire() {
    // The delay-load SEH gate is a separate function and cannot reenter this
    // owner. Keep its whole transition serialized with the last Release.
    std::lock_guard<std::mutex> lock(Mutex());
    if (InitializedFlag()) { ++RefCount(); return true; }
    const HRESULT hr = DelaySafeMFStartup();
    if (SUCCEEDED(hr)) { InitializedFlag() = true; ++RefCount(); ++Stats().startups; return true; }
    return false;
}

void MediaFoundation::Release() {
    std::lock_guard<std::mutex> lock(Mutex());
    if (RefCount() > 0) { --RefCount(); }
    if (RefCount() == 0 && InitializedFlag()) {
        DelaySafeMFShutdown();
        ++Stats().shutdowns;
        InitializedFlag() = false;
    }
}

bool MediaFoundation::IsInitialized() { std::lock_guard<std::mutex> lock(Mutex()); return InitializedFlag(); }

void MediaFoundation::ShutdownIfInitialized() {
    std::lock_guard<std::mutex> lock(Mutex());
    if (InitializedFlag()) {
        DelaySafeMFShutdown();
        ++Stats().shutdowns;
        InitializedFlag() = false;
        RefCount() = 0;
    }
}

} // namespace Aperture
