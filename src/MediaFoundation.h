#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>

#include <mutex>
#include <cstdint>

namespace Aperture {

// Reference-counted, lazy Media Foundation startup.
//
// The hub must not pay for MFStartup (or the MF DLL mapping, once
// mfreadwrite/mfplat are delay-loaded) before any import/metadata work exists.
// Import and metadata-backfill workers acquire for the whole job; single
// synchronous probes acquire per call via ScopedAcquire (nesting is safe). Shutdown balances only when initialized, so a hub that never
// probed anything shuts down without touching MF at all.
class MediaFoundation {
public:
    // Returns true when MF is usable (startup succeeded). Nested acquires
    // only bump the count. Never throws; delay-load failures yield false.
    static bool Acquire();
    static void Release();
    static bool IsInitialized();
    struct LifecycleStats { uint64_t startups = 0, shutdowns = 0; int references = 0; };
    static LifecycleStats GetLifecycleStats();
    // For process teardown after workers have joined. Shuts down only when
    // initialized; safe to call when never acquired.
    static void ShutdownIfInitialized();
    class ScopedAcquire {
    public:
        ScopedAcquire() : held_(Acquire()) {}
        ~ScopedAcquire() { if (held_) Release(); }
        ScopedAcquire(const ScopedAcquire&) = delete;
        ScopedAcquire& operator=(const ScopedAcquire&) = delete;
        bool held() const { return held_; }
    private:
        bool held_ = false;
    };

private:
    static std::mutex& Mutex();
    static int& RefCount();
    static bool& InitializedFlag();
    static LifecycleStats& Stats();
};

} // namespace Aperture
