#include "MediaProbeIsolation.h"
#include <shellapi.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <cmath>
#include <stdexcept>
#include <psapi.h>
#include <iomanip>

using namespace Aperture;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::wstring TempFile(const wchar_t* suffix) {
    wchar_t dir[MAX_PATH];
    Require(GetTempPathW(MAX_PATH, dir) != 0, "Temp path");
    return std::wstring(dir) + L"Aperture_probe_" + std::to_wstring(GetCurrentProcessId()) + suffix;
}
void WriteWave(const std::wstring& path) {
    std::ofstream file(path, std::ios::binary);
    const uint32_t dataSize = 16000 * 2 * 2, riffSize = dataSize + 36, fmtSize = 16;
    const uint16_t pcm = 1, channels = 1, align = 2, bits = 16;
    const uint32_t rate = 16000, byteRate = 32000;
    file.write("RIFF", 4); file.write(reinterpret_cast<const char*>(&riffSize), 4); file.write("WAVEfmt ", 8);
    file.write(reinterpret_cast<const char*>(&fmtSize), 4); file.write(reinterpret_cast<const char*>(&pcm), 2);
    file.write(reinterpret_cast<const char*>(&channels), 2); file.write(reinterpret_cast<const char*>(&rate), 4);
    file.write(reinterpret_cast<const char*>(&byteRate), 4); file.write(reinterpret_cast<const char*>(&align), 2);
    file.write(reinterpret_cast<const char*>(&bits), 2); file.write("data", 4); file.write(reinterpret_cast<const char*>(&dataSize), 4);
    std::vector<char> samples(dataSize); file.write(samples.data(), samples.size());
    Require(file.good(), "Write wave fixture");
}
void WriteBitmap(const std::wstring& path) {
    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header); header.biWidth = 128; header.biHeight = 64;
    header.biPlanes = 1; header.biBitCount = 32; header.biCompression = BI_RGB;
    header.biSizeImage = 128 * 64 * 4;
    fileHeader.bfType = 0x4D42; fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(header);
    fileHeader.bfSize = fileHeader.bfOffBits + header.biSizeImage;
    std::vector<uint32_t> pixels(128 * 64, 0xFF2255BB);
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(pixels.data()), pixels.size() * 4);
    Require(file.good(), "Write bitmap fixture");
}
void RequireProcessExited(DWORD pid) {
    Require(pid != 0, "Helper pid was observed");
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) { Require(GetLastError() == ERROR_INVALID_PARAMETER, "Open terminated helper"); return; }
    const DWORD result = WaitForSingleObject(process, 2000);
    CloseHandle(process);
    Require(result == WAIT_OBJECT_0, "Killed helper exits independently of parent owner");
}
double NowMs() {
    LARGE_INTEGER time, frequency;
    QueryPerformanceCounter(&time); QueryPerformanceFrequency(&frequency);
    return double(time.QuadPart) * 1000.0 / frequency.QuadPart;
}
PROCESS_MEMORY_COUNTERS_EX ProcessMemory(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    Require(process != nullptr, "Open live helper for resource measurement");
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
    const bool result = GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE;
    CloseHandle(process);
    Require(result, "Read helper resource counters");
    return memory;
}
void ResourceProbe(const std::wstring& wave, const std::wstring& bitmap, const std::wstring& executable) {
    std::vector<std::unique_ptr<MediaProbeIsolation>> helpers;
    std::vector<DWORD> ids;
    double duration = 0; uint32_t width = 0, height = 0;
    helpers.push_back(std::make_unique<MediaProbeIsolation>(MediaProbeIsolation::CancelCheck{}, executable));
    const double coldStart = NowMs();
    Require(helpers.front()->ProbeMetadata(wave, duration, width, height), "Measured first metadata probe");
    const double coldMs = NowMs() - coldStart;
    const auto firstMemory = ProcessMemory(helpers.front()->HelperProcessId());
    const double reusedStart = NowMs();
    Require(helpers.front()->ProbeMetadata(wave, duration, width, height), "Measured reused metadata probe");
    const double reusedMs = NowMs() - reusedStart;
    std::cout << "resource-first-metadata-ms,pass," << std::fixed << std::setprecision(3) << coldMs << '\n';
    std::cout << "resource-reused-metadata-ms,pass," << reusedMs << '\n';
    std::cout << "resource-single-helper-private-bytes,pass," << firstMemory.PrivateUsage << '\n';
    std::cout << "resource-single-helper-working-bytes,pass," << firstMemory.WorkingSetSize << '\n';
    helpers.clear();
    uint64_t privateBytes = 0;
    std::vector<uint8_t> pixels;
    for (int i = 0; i < 4; ++i) {
        helpers.push_back(std::make_unique<MediaProbeIsolation>(MediaProbeIsolation::CancelCheck{}, executable));
        Require(helpers.back()->GetThumbnail(bitmap, 320, 180, pixels, width, height), "Measured Shell-only helper owner");
        privateBytes += ProcessMemory(helpers.back()->HelperProcessId()).PrivateUsage;
    }
    for (const auto& helper : helpers) ids.push_back(helper->HelperProcessId());
    std::cout << "resource-four-shell-helpers-private-bytes,pass," << privateBytes << '\n';
    // Exercise actual idle child-process termination, not only an owner flag.
    const double idleStart = NowMs();
    for (const DWORD pid : ids) {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        Require(process != nullptr, "Open idle helper");
        const DWORD result = WaitForSingleObject(process, 6500);
        CloseHandle(process);
        Require(result == WAIT_OBJECT_0, "Idle helper releases process resources without new UI work");
    }
    std::cout << "resource-idle-exit-ms,pass," << NowMs() - idleStart << '\n';
    Require(helpers.front()->ProbeMetadata(wave, duration, width, height) && helpers.front()->HelperProcessId() != ids.front(),
        "A new request restarts an idle helper");
    std::cout << "idle-exit-and-restart,pass,0\n";
}
}
int wmain(int argc, wchar_t** argv) {
    if (IsMediaProbeHelperCommand(argc, argv)) return RunMediaProbeHelper(argc, argv);
    const std::wstring wave = TempFile(L".wav"), bitmap = TempFile(L".bmp");
    int result = 0;
    const std::wstring resourceExecutable = argc == 3 && wcscmp(argv[1], L"--resource-helper-executable") == 0 ? argv[2] : L"";
    std::cout << "case,status,value\n";
    try {
        WriteWave(wave); WriteBitmap(bitmap);
        {
            MediaProbeIsolation probes;
            double duration = 0; uint32_t width = 0, height = 0;
            Require(probes.ProbeMetadata(wave, duration, width, height), "MF wave metadata in helper");
            Require(std::abs(duration - 2.0) < 0.02 && width == 0 && height == 0, "MF wave duration preserved");
            const DWORD pid = probes.HelperProcessId();
            Require(probes.ProbeMetadata(wave, duration, width, height) && probes.HelperProcessId() == pid,
                "Consecutive metadata probes reuse one helper");
            std::cout << "metadata-and-reuse,pass,0\n";
            std::vector<uint8_t> pixels;
            Require(probes.GetThumbnail(bitmap, 32, 18, pixels, width, height), "Shell bitmap thumbnail in helper");
            Require(width > 0 && height > 0 && width <= 32 && height <= 18 && pixels.size() == size_t(width) * height * 4,
                "Shell cubic downscale and BGRA bounds preserved");
            Require(probes.HelperProcessId() == pid, "Metadata and Shell share worker-owned helper");
            std::cout << "shell-thumbnail-and-bounds,pass,0\n";
        }
        {
            MediaProbeIsolation probes;
            DWORD handlesBefore = 0, handlesAfter = 0;
            Require(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore) != FALSE,
                "Read parent handles before explicit idle release");
            for (int cycle = 0; cycle < 3; ++cycle) {
                double duration = 0; uint32_t width = 0, height = 0;
                std::vector<uint8_t> pixels;
                Require(probes.ProbeMetadata(wave, duration, width, height), "Metadata restarts after idle release");
                Require(std::abs(duration - 2.0) < 0.02, "Restarted metadata preserves duration");
                Require(probes.GetThumbnail(bitmap, 32, 18, pixels, width, height), "Thumbnail restarts after idle release");
                Require(width && height && pixels.size() == size_t(width) * height * 4, "Restarted thumbnail bytes are complete");
                const DWORD pid = probes.HelperProcessId();
                probes.ReleaseIdleResources();
                Require(!probes.HelperProcessId() && probes.LastOutcome() == MediaProbeIsolation::Outcome::Success,
                    "Idle release clears owner while retaining last outcome");
                RequireProcessExited(pid);
                probes.ReleaseIdleResources();
                Require(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter) != FALSE, "Read released parent handles");
                Require(handlesAfter <= handlesBefore, "Idle release closes parent job, mapping and event handles");
            }
            std::cout << "explicit-idle-release-and-restart,pass,0\n";
        }
        {
            MediaProbeIsolation probes;
            const ULONGLONG started = GetTickCount64();
            Require(!probes.RunBlockedProbeForTesting(500), "Blocked helper deadline fails");
            const ULONGLONG elapsed = GetTickCount64() - started;
            Require(probes.LastOutcome() == MediaProbeIsolation::Outcome::TimedOut && probes.LastProbeEnteredForTesting(),
                "Deadline interrupts a confirmed entered provider");
            Require(elapsed < 2000, "Provider timeout is bounded");
            const DWORD killed = probes.LastHelperProcessIdForTesting();
            RequireProcessExited(killed);
            double duration = 0; uint32_t width = 0, height = 0;
            Require(probes.ProbeMetadata(wave, duration, width, height) && probes.HelperProcessId() != killed,
                "Worker recovers with fresh helper after timeout");
            std::cout << "blocked-provider-timeout-and-recovery,pass," << elapsed << '\n';
        }
        {
            MediaProbeIsolation* owner = nullptr;
            MediaProbeIsolation probes([&]() { return owner && owner->IsCurrentProbeEnteredForTesting(); });
            owner = &probes;
            const ULONGLONG started = GetTickCount64();
            Require(!probes.RunBlockedProbeForTesting(10000), "Entered provider canceled");
            const ULONGLONG elapsed = GetTickCount64() - started;
            Require(probes.LastOutcome() == MediaProbeIsolation::Outcome::Canceled && probes.LastProbeEnteredForTesting(),
                "Cancellation interrupts confirmed blocked helper");
            Require(elapsed < 2000, "Cancellation does not wait for 10-second provider deadline");
            RequireProcessExited(probes.LastHelperProcessIdForTesting());
            std::cout << "blocked-provider-cancellation,pass," << elapsed << '\n';
        }
        {
            MediaProbeIsolation probes;
            double duration = 9; uint32_t width = 9, height = 9;
            Require(!probes.ProbeMetadata(std::wstring(32768, L'x'), duration, width, height) && !probes.HelperProcessId(),
                "Oversized path rejected before child launch");
            std::vector<uint8_t> pixels;
            Require(!probes.GetThumbnail(bitmap, UINT32_MAX, UINT32_MAX, pixels, width, height), "Oversized pixel request rejected");
            wchar_t command[] = L"--media-probe-helper", invalid[] = L"-1", zero[] = L"0";
            wchar_t* invalidArgs[] = { argv[0], command, invalid, zero, zero };
            Require(RunMediaProbeHelper(5, invalidArgs) == ERROR_INVALID_HANDLE, "Internal handles fully validated");
            std::cout << "input-validation,pass,0\n";
        }
        {
            DWORD handlesBefore = 0, handlesAfter = 0;
            // Warm the Windows failed-launch path before measuring persistent
            // handle growth (CreateProcess can lazily load AppCompat state).
            {
                MediaProbeIsolation unavailable({}, TempFile(L"_missing.exe"));
                double duration = 0; uint32_t width = 0, height = 0;
                Require(!unavailable.ProbeMetadata(wave, duration, width, height), "Warm failed helper launch");
            }
            Require(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore) != FALSE, "Read parent handles before failed launches");
            for (int i = 0; i < 20; ++i) {
                MediaProbeIsolation unavailable({}, TempFile(L"_missing.exe"));
                double duration = 9; uint32_t width = 9, height = 9;
                Require(!unavailable.ProbeMetadata(wave, duration, width, height) &&
                    unavailable.LastOutcome() == MediaProbeIsolation::Outcome::Unavailable && !unavailable.HelperProcessId() &&
                    duration == 0 && width == 0 && height == 0, "Failed helper launch preserves explicit failure and cleared output");
            }
            Require(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter) != FALSE, "Read handles after failed launches");
            if (handlesAfter > handlesBefore) std::cerr << "Parent handles before=" << handlesBefore << " after=" << handlesAfter << '\n';
            Require(handlesAfter <= handlesBefore,
                "Failed launches release all job, mapping and event handles");
            std::cout << "helper-unavailable-and-handle-cleanup,pass,0\n";
        }
        {
            double duration = 9; uint32_t width = 9, height = 9; bool handled = true;
            Require(!TryIsolatedMetadataProbe(wave, duration, width, height, handled) && !handled,
                "Direct component path unchanged outside override");
            ScopedMediaProbeIsolation canceled([]() { return true; });
            Require(!TryIsolatedMetadataProbe(wave, duration, width, height, handled) && handled,
                "Thread-local override observes cancellation before launching");
            std::cout << "scoped-probe-routing,pass,0\n";
        }
        ResourceProbe(wave, bitmap, resourceExecutable);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    DeleteFileW(wave.c_str()); DeleteFileW(bitmap.c_str());
    return result;
}
