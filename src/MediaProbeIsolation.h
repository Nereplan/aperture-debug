#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Aperture {

// MF source readers and third-party Shell thumbnail providers have no
// synchronous cancellation contract. Keep those calls in a disposable child
// process instead of an App/Renderer thread. One child is reused per worker;
// its handles, shared memory and job object never contain an App pointer.
class MediaProbeIsolation {
public:
    enum class Outcome { Success, Failed, Canceled, TimedOut, Unavailable };
    using CancelCheck = std::function<bool()>;
    explicit MediaProbeIsolation(CancelCheck canceled = {}, std::wstring executable = {});
    ~MediaProbeIsolation();
    MediaProbeIsolation(const MediaProbeIsolation&) = delete;
    MediaProbeIsolation& operator=(const MediaProbeIsolation&) = delete;

    bool ProbeMetadata(const std::wstring& path, double& duration, uint32_t& width, uint32_t& height,
                       DWORD timeoutMs = 30000);
    bool GetThumbnail(const std::wstring& path, uint32_t requestedWidth, uint32_t requestedHeight,
                      std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height, DWORD timeoutMs = 15000);
    Outcome LastOutcome() const;
    DWORD HelperProcessId() const;
    // The owner calls this between requests; the next probe starts lazily.
    void ReleaseIdleResources();
#ifdef APERTURE_TEST_PROBE_ISOLATION
    bool RunBlockedProbeForTesting(DWORD timeoutMs);
    bool IsCurrentProbeEnteredForTesting() const;
    bool LastProbeEnteredForTesting() const;
    DWORD LastHelperProcessIdForTesting() const;
#endif
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Import/backfill install this thread-local override. Ordinary component
// callers retain their existing direct probe behavior; helper-side probes
// never recurse into a new helper.
class ScopedMediaProbeIsolation : public MediaProbeIsolation {
public:
    explicit ScopedMediaProbeIsolation(CancelCheck canceled);
    ~ScopedMediaProbeIsolation();
private:
    MediaProbeIsolation* previous = nullptr;
};
bool TryIsolatedMetadataProbe(const std::wstring& path, double& duration, uint32_t& width, uint32_t& height,
                              bool& handled);
bool IsMediaProbeHelperCommand(int argc, wchar_t** argv);
int RunMediaProbeHelper(int argc, wchar_t** argv);

} // namespace Aperture
