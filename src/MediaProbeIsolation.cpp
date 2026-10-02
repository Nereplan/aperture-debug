#include "MediaProbeIsolation.h"
#include "Common.h"
#include "DllLoader.h"
#include "MediaFoundation.h"
#include <mfreadwrite.h>
#include <shlobj.h>
#include <wincodec.h>
#include <cerrno>
#include <cmath>
#include <cwchar>
#include <limits>

namespace Aperture {
namespace {
constexpr uint32_t kProtocolMagic = 0x50524141; // AARP
constexpr uint32_t kProtocolVersion = 1;
constexpr size_t kMaxPathCharacters = 32768;
constexpr size_t kMaxPixelBytes = 4 * 1024 * 1024;
constexpr DWORD kCancellationPollMs = 25;
constexpr DWORD kIdleHelperExitMs = 5000;
enum class Operation : uint32_t { Metadata = 1, Thumbnail = 2, Stop = 3,
#ifdef APERTURE_TEST_PROBE_ISOLATION
    BlockForTesting = 4,
#endif
};
struct SharedProbe {
    uint32_t magic = kProtocolMagic, version = kProtocolVersion;
    uint64_t sequence = 0, completedSequence = 0;
    uint32_t operation = 0, success = 0, requestedWidth = 0, requestedHeight = 0;
    uint32_t width = 0, height = 0, pixelBytes = 0, reserved = 0;
    double duration = 0;
    wchar_t path[kMaxPathCharacters] = {};
    uint8_t pixels[kMaxPixelBytes] = {};
};
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value; }
    void reset(HANDLE replacement = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = replacement;
    }
private:
    HANDLE value = nullptr;
};
thread_local MediaProbeIsolation* currentIsolation = nullptr;

HRESULT CreateAttributes(IMFAttributes** attrs) {
    *attrs = nullptr;
    HRESULT hr = E_FAIL;
    __try { hr = MFCreateAttributes(attrs, 1); }
    __except (DelayLoadExceptionFilter(GetExceptionCode())) { *attrs = nullptr; }
    return hr;
}
HRESULT CreateReader(const wchar_t* path, IMFAttributes* attrs, IMFSourceReader** reader) {
    *reader = nullptr;
    HRESULT hr = E_FAIL;
    __try { hr = MFCreateSourceReaderFromURL(path, attrs, reader); }
    __except (DelayLoadExceptionFilter(GetExceptionCode())) { *reader = nullptr; }
    return hr;
}
bool ReadMetadata(SharedProbe& request) {
    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    if (FAILED(CreateAttributes(&attrs)) || !attrs) return false;
    if (FAILED(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE))) return false;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    if (FAILED(CreateReader(request.path, attrs.Get(), &reader)) || !reader) return false;
    PROPVARIANT duration;
    PropVariantInit(&duration);
    const HRESULT hr = reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration);
    if (SUCCEEDED(hr) && duration.vt == VT_UI8) request.duration = static_cast<double>(duration.uhVal.QuadPart) / 10000000.0;
    PropVariantClear(&duration);
    Microsoft::WRL::ComPtr<IMFMediaType> type;
    if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &type)) && type)
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &request.width, &request.height);
    return true;
}
bool ReadThumbnail(SharedProbe& request, IWICImagingFactory* wic) {
    if (!wic || request.requestedWidth == 0 || request.requestedHeight == 0 ||
        request.requestedWidth > 4096 || request.requestedHeight > 4096 ||
        uint64_t(request.requestedWidth) * request.requestedHeight * 4 > kMaxPixelBytes) return false;
    Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(request.path, nullptr, IID_PPV_ARGS(&factory))) || !factory) return false;
    const SIZE size{ static_cast<LONG>(request.requestedWidth), static_cast<LONG>(request.requestedHeight) };
    HBITMAP bitmap = nullptr;
    HRESULT hr = factory->GetImage(size, SIIGBF_INCACHEONLY | SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY, &bitmap);
    if (FAILED(hr) || !bitmap) {
        if (bitmap) { DeleteObject(bitmap); bitmap = nullptr; }
        hr = factory->GetImage(size, SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY, &bitmap);
    }
    if (FAILED(hr) || !bitmap) { if (bitmap) DeleteObject(bitmap); return false; }
    Microsoft::WRL::ComPtr<IWICBitmap> wicBitmap;
    hr = wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &wicBitmap);
    DeleteObject(bitmap);
    if (FAILED(hr) || !wicBitmap) return false;
    UINT width = 0, height = 0;
    if (FAILED(wicBitmap->GetSize(&width, &height)) || !width || !height) return false;
    const double scale = std::min({ 1.0, double(request.requestedWidth) / width, double(request.requestedHeight) / height });
    const UINT targetWidth = std::max(1u, static_cast<UINT>(std::round(width * scale)));
    const UINT targetHeight = std::max(1u, static_cast<UINT>(std::round(height * scale)));
    Microsoft::WRL::ComPtr<IWICBitmapSource> source = wicBitmap;
    if (targetWidth != width || targetHeight != height) {
        Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(wicBitmap.Get(), targetWidth, targetHeight, WICBitmapInterpolationModeHighQualityCubic))) return false;
        source = scaler;
    }
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(source.Get(),
        GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    if (FAILED(converter->GetSize(&width, &height)) || !width || !height || width > request.requestedWidth ||
        height > request.requestedHeight || uint64_t(width) * height * 4 > kMaxPixelBytes) return false;
    const UINT bytes = width * height * 4;
    if (FAILED(converter->CopyPixels(nullptr, width * 4, bytes, request.pixels))) return false;
    request.width = width; request.height = height; request.pixelBytes = bytes;
    return true;
}
bool ParseHandle(const wchar_t* text, HANDLE& handle) {
    if (!text || !*text || *text == L'-' || *text == L'+') return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long value = wcstoull(text, &end, 10);
    if (errno || !end || *end || !value || value > std::numeric_limits<uintptr_t>::max()) return false;
    handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
    DWORD flags = 0;
    return GetHandleInformation(handle, &flags) != FALSE;
}
}

struct MediaProbeIsolation::Impl {
    CancelCheck canceled;
    std::wstring executable;
    Handle job, process, mapping, requestEvent, responseEvent;
    SharedProbe* shared = nullptr;
    Outcome outcome = Outcome::Unavailable;
    DWORD pid = 0;
#ifdef APERTURE_TEST_PROBE_ISOLATION
    DWORD lastPid = 0;
    bool probeEntered = false;
#endif
    uint64_t sequence = 0;
    explicit Impl(CancelCheck check, std::wstring path) : canceled(std::move(check)), executable(std::move(path)) {}
    ~Impl() { Stop(true); }
    bool IsCanceled() const { return canceled && canceled(); }
    void Stop(bool graceful = false) {
#ifdef APERTURE_TEST_PROBE_ISOLATION
        if (shared) probeEntered = InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&shared->reserved), 0, 0) != 0;
#endif
        if (graceful && shared && process.get() && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT) {
            shared->operation = static_cast<uint32_t>(Operation::Stop);
            SetEvent(requestEvent.get());
            // Let normal MF/COM teardown balance its acquisitions, while a
            // blocked provider still cannot hold parent teardown indefinitely.
            WaitForSingleObject(process.get(), 250);
        }
        // Closing this non-inherited job terminates the isolated process even
        // when it is stuck inside an MF codec or a Shell extension. Its own
        // address space can be destroyed without touching parent C++ objects.
        job.reset();
        if (process.get()) {
            // A failed job assignment must never leave a suspended orphan.
            if (WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT) TerminateProcess(process.get(), ERROR_CANCELLED);
            process.reset();
        }
        if (shared) { UnmapViewOfFile(shared); shared = nullptr; }
        mapping.reset(); requestEvent.reset(); responseEvent.reset(); pid = 0;
    }
    bool Start() {
        if (process.get() && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT) return true;
        Stop();
        if (IsCanceled()) { outcome = Outcome::Canceled; return false; }
        if (executable.empty()) {
            std::vector<wchar_t> path(32768);
            const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (!size || size >= path.size()) return false;
            executable.assign(path.data(), size);
        }
        SECURITY_ATTRIBUTES attributes{ sizeof(attributes), nullptr, TRUE };
        mapping.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, sizeof(SharedProbe), nullptr));
        requestEvent.reset(CreateEventW(&attributes, FALSE, FALSE, nullptr));
        responseEvent.reset(CreateEventW(&attributes, FALSE, FALSE, nullptr));
        job.reset(CreateJobObjectW(nullptr, nullptr));
        if (!mapping.get() || !requestEvent.get() || !responseEvent.get() || !job.get()) { Stop(); return false; }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { Stop(); return false; }
        shared = static_cast<SharedProbe*>(MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedProbe)));
        if (!shared) { Stop(); return false; }
        // Only the small protocol header needs initialization. Pixel pages
        // remain demand paged until an actual thumbnail is returned.
        shared->magic = kProtocolMagic; shared->version = kProtocolVersion;
        shared->sequence = 0; shared->completedSequence = 0;
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<uint8_t> attributeStorage(attributeBytes);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes)) { Stop(); return false; }
        const HANDLE inherited[] = { mapping.get(), requestEvent.get(), responseEvent.get() };
        const bool attributesOk = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE*>(inherited), sizeof(inherited), nullptr, nullptr) != FALSE;
        const auto number = [](HANDLE handle) { return std::to_wstring(reinterpret_cast<uintptr_t>(handle)); };
        std::wstring command = L"\"" + executable + L"\" --media-probe-helper " + number(mapping.get()) + L" " +
            number(requestEvent.get()) + L" " + number(responseEvent.get());
        PROCESS_INFORMATION info{};
        const bool created = attributesOk && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &info);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (!created) { Stop(); return false; }
        process.reset(info.hProcess);
        Handle thread(info.hThread);
        pid = info.dwProcessId;
#ifdef APERTURE_TEST_PROBE_ISOLATION
        lastPid = pid;
#endif
        if (!AssignProcessToJobObject(job.get(), process.get()) || ResumeThread(thread.get()) == DWORD(-1)) { Stop(); return false; }
        return true;
    }
    bool Run(Operation operation, const std::wstring& path, uint32_t width, uint32_t height, DWORD timeoutMs, bool retry = true) {
        outcome = Outcome::Unavailable;
        if (IsCanceled()) { outcome = Outcome::Canceled; Stop(); return false; }
        if (path.size() >= kMaxPathCharacters || path.find(L'\0') != std::wstring::npos || !timeoutMs) return false;
        if (!Start()) return false;
        ResetEvent(responseEvent.get());
        shared->sequence = ++sequence; shared->completedSequence = 0;
        shared->operation = static_cast<uint32_t>(operation); shared->success = 0;
        shared->requestedWidth = width; shared->requestedHeight = height;
        shared->width = 0; shared->height = 0; shared->duration = 0; shared->pixelBytes = 0;
        shared->reserved = 0;
        std::copy(path.begin(), path.end(), shared->path); shared->path[path.size()] = L'\0';
        if (!SetEvent(requestEvent.get())) { Stop(); return false; }
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        const HANDLE waits[] = { responseEvent.get(), process.get() };
        while (true) {
            if (IsCanceled()) { outcome = Outcome::Canceled; Stop(); return false; }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) { outcome = Outcome::TimedOut; Stop(); return false; }
            const DWORD result = WaitForMultipleObjects(2, waits, FALSE,
                static_cast<DWORD>(std::min<ULONGLONG>(kCancellationPollMs, deadline - now)));
            if (result == WAIT_OBJECT_0) {
                if (shared->magic != kProtocolMagic || shared->version != kProtocolVersion ||
                    shared->completedSequence != sequence) { Stop(); return false; }
                outcome = shared->success ? Outcome::Success : Outcome::Failed;
                return shared->success != 0;
            }
            if (result == WAIT_OBJECT_0 + 1 && retry && !IsCanceled()) {
                // An idle helper can exit between Start's liveness check and
                // publishing the next request. Retry once with a new process.
                Stop(); return Run(operation, path, width, height, timeoutMs, false);
            }
            if (result != WAIT_TIMEOUT) { Stop(); return false; }
        }
    }
};

MediaProbeIsolation::MediaProbeIsolation(CancelCheck canceled, std::wstring executable)
    : impl(std::make_unique<Impl>(std::move(canceled), std::move(executable))) {}
MediaProbeIsolation::~MediaProbeIsolation() = default;
MediaProbeIsolation::Outcome MediaProbeIsolation::LastOutcome() const { return impl->outcome; }
DWORD MediaProbeIsolation::HelperProcessId() const { return impl->pid; }
bool MediaProbeIsolation::ProbeMetadata(const std::wstring& path, double& duration, uint32_t& width, uint32_t& height, DWORD timeoutMs) {
    duration = 0; width = 0; height = 0;
    if (!impl->Run(Operation::Metadata, path, 0, 0, timeoutMs)) return false;
    if (!std::isfinite(impl->shared->duration) || impl->shared->duration < 0) { impl->outcome = Outcome::Failed; return false; }
    duration = impl->shared->duration; width = impl->shared->width; height = impl->shared->height;
    return true;
}
bool MediaProbeIsolation::GetThumbnail(const std::wstring& path, uint32_t requestedWidth, uint32_t requestedHeight,
    std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height, DWORD timeoutMs) {
    pixels.clear(); width = 0; height = 0;
    if (!requestedWidth || !requestedHeight || requestedWidth > 4096 || requestedHeight > 4096 ||
        uint64_t(requestedWidth) * requestedHeight * 4 > kMaxPixelBytes ||
        !impl->Run(Operation::Thumbnail, path, requestedWidth, requestedHeight, timeoutMs)) return false;
    const auto& result = *impl->shared;
    if (!result.width || !result.height || result.width > requestedWidth || result.height > requestedHeight ||
        uint64_t(result.width) * result.height * 4 != result.pixelBytes || result.pixelBytes > kMaxPixelBytes) {
        impl->outcome = Outcome::Failed; return false;
    }
    pixels.assign(result.pixels, result.pixels + result.pixelBytes);
    width = result.width; height = result.height;
    return true;
}
#ifdef APERTURE_TEST_PROBE_ISOLATION
bool MediaProbeIsolation::RunBlockedProbeForTesting(DWORD timeoutMs) {
    return impl->Run(Operation::BlockForTesting, L"", 0, 0, timeoutMs);
}
bool MediaProbeIsolation::IsCurrentProbeEnteredForTesting() const {
    return impl->shared && InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&impl->shared->reserved), 0, 0) != 0;
}
bool MediaProbeIsolation::LastProbeEnteredForTesting() const { return impl->probeEntered; }
DWORD MediaProbeIsolation::LastHelperProcessIdForTesting() const { return impl->lastPid; }
#endif
ScopedMediaProbeIsolation::ScopedMediaProbeIsolation(CancelCheck canceled) : MediaProbeIsolation(std::move(canceled)) {
    previous = currentIsolation; currentIsolation = this;
}
ScopedMediaProbeIsolation::~ScopedMediaProbeIsolation() { currentIsolation = previous; }
bool TryIsolatedMetadataProbe(const std::wstring& path, double& duration, uint32_t& width, uint32_t& height, bool& handled) {
    handled = currentIsolation != nullptr;
    return handled && currentIsolation->ProbeMetadata(path, duration, width, height);
}
bool IsMediaProbeHelperCommand(int argc, wchar_t** argv) {
    return argv && argc > 1 && argv[1] && wcscmp(argv[1], L"--media-probe-helper") == 0;
}
int RunMediaProbeHelper(int argc, wchar_t** argv) {
    if (!IsMediaProbeHelperCommand(argc, argv) || argc != 5) return ERROR_INVALID_PARAMETER;
    HANDLE mappingHandle = nullptr, requestHandle = nullptr, responseHandle = nullptr;
    if (!ParseHandle(argv[2], mappingHandle) || !ParseHandle(argv[3], requestHandle) || !ParseHandle(argv[4], responseHandle))
        return ERROR_INVALID_HANDLE;
    Handle mapping(mappingHandle), requestEvent(requestHandle), responseEvent(responseHandle);
    auto* shared = static_cast<SharedProbe*>(MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedProbe)));
    if (!shared) return ERROR_INVALID_DATA;
    DllLoader::HardenProcessDllSearch();
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int exitCode = 0;
    {
        std::unique_ptr<MediaFoundation::ScopedAcquire> mf;
        Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
        while (WaitForSingleObject(requestEvent.get(), kIdleHelperExitMs) == WAIT_OBJECT_0) {
            if (shared->magic != kProtocolMagic || shared->version != kProtocolVersion ||
                !wmemchr(shared->path, L'\0', kMaxPathCharacters)) { exitCode = ERROR_INVALID_DATA; break; }
            const Operation operation = static_cast<Operation>(shared->operation);
            if (operation == Operation::Stop) break;
            bool success = false;
            try {
                if (operation == Operation::Metadata && SUCCEEDED(coHr)) {
                    if (!mf) mf = std::make_unique<MediaFoundation::ScopedAcquire>();
                    if (mf->held()) success = ReadMetadata(*shared);
                } else if (operation == Operation::Thumbnail && SUCCEEDED(coHr)) {
                    if (!wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
                    success = ReadThumbnail(*shared, wic.Get());
#ifdef APERTURE_TEST_PROBE_ISOLATION
                } else if (operation == Operation::BlockForTesting) {
                    InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared->reserved), 1);
                    Sleep(INFINITE);
#endif
                }
            } catch (...) { success = false; }
            shared->success = success ? 1u : 0u; shared->completedSequence = shared->sequence;
            if (!SetEvent(responseEvent.get())) { exitCode = ERROR_INVALID_HANDLE; break; }
        }
    }
    if (SUCCEEDED(coHr)) CoUninitialize();
    UnmapViewOfFile(shared);
    return exitCode;
}
} // namespace Aperture
