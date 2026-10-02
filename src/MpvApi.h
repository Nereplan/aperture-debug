#pragma once

#include "DllLoader.h"
#include "include/mpv/client.h"
#include "include/mpv/render.h"
#include <functional>

namespace Aperture {

enum class MpvApiProfile { Playback, SoftwareRenderer };
enum class MpvOptionRequirement { Optional, Essential };

struct MpvInitializationDiagnostic {
    std::string message;
    int errorCode = 0;
    bool essential = false;
};

// The signatures come from the shipped API headers, including calling conventions.
// Each owner holds its own DLL reference; client/render handles remain independent.
#define APERTURE_MPV_API_FUNCTIONS(X) \
    X(mpv_create) X(mpv_initialize) X(mpv_destroy) X(mpv_terminate_destroy) \
    X(mpv_command) X(mpv_command_string) X(mpv_set_option) X(mpv_set_option_string) \
    X(mpv_get_property) X(mpv_get_property_string) X(mpv_set_property) X(mpv_set_property_string) \
    X(mpv_free) X(mpv_observe_property) X(mpv_wait_event) X(mpv_wakeup) X(mpv_error_string) \
    X(mpv_render_context_create) X(mpv_render_context_set_update_callback) \
    X(mpv_render_context_update) X(mpv_render_context_render) X(mpv_render_context_free)

struct MpvApiFunctions {
#define APERTURE_DECLARE_MPV_FUNCTION(name) decltype(&::name) fn_##name = nullptr;
    APERTURE_MPV_API_FUNCTIONS(APERTURE_DECLARE_MPV_FUNCTION)
#undef APERTURE_DECLARE_MPV_FUNCTION

    using SymbolResolver = FARPROC (WINAPI*)(HMODULE, LPCSTR);

    void Clear() {
#define APERTURE_CLEAR_MPV_FUNCTION(name) fn_##name = nullptr;
        APERTURE_MPV_API_FUNCTIONS(APERTURE_CLEAR_MPV_FUNCTION)
#undef APERTURE_CLEAR_MPV_FUNCTION
    }

    bool Bind(HMODULE module, MpvApiProfile profile, std::string& missing,
              SymbolResolver resolve = nullptr) {
        Clear();
        missing.clear();
        // Production uses the Win32 import directly; explicit resolvers remain
        // available for deterministic export-failure tests.
#define APERTURE_BIND_MPV_FUNCTION(name) fn_##name = reinterpret_cast<decltype(fn_##name)>(resolve ? resolve(module, #name) : GetProcAddress(module, #name));
        APERTURE_MPV_API_FUNCTIONS(APERTURE_BIND_MPV_FUNCTION)
#undef APERTURE_BIND_MPV_FUNCTION
        auto require = [&](bool present, const char* name) {
            if (!present) { if (!missing.empty()) missing += ", "; missing += name; }
        };
#define APERTURE_REQUIRE_MPV_FUNCTION(name) require(fn_##name != nullptr, #name)
        APERTURE_REQUIRE_MPV_FUNCTION(mpv_create);
        APERTURE_REQUIRE_MPV_FUNCTION(mpv_initialize);
        APERTURE_REQUIRE_MPV_FUNCTION(mpv_terminate_destroy);
        APERTURE_REQUIRE_MPV_FUNCTION(mpv_command);
        APERTURE_REQUIRE_MPV_FUNCTION(mpv_set_option_string);
        APERTURE_REQUIRE_MPV_FUNCTION(mpv_set_property_string);
        if (profile == MpvApiProfile::Playback) {
            APERTURE_REQUIRE_MPV_FUNCTION(mpv_set_option);
            APERTURE_REQUIRE_MPV_FUNCTION(mpv_get_property);
            APERTURE_REQUIRE_MPV_FUNCTION(mpv_get_property_string);
            APERTURE_REQUIRE_MPV_FUNCTION(mpv_set_property);
            APERTURE_REQUIRE_MPV_FUNCTION(mpv_free);
            APERTURE_REQUIRE_MPV_FUNCTION(mpv_observe_property);
            APERTURE_REQUIRE_MPV_FUNCTION(mpv_wait_event);
        } else {
            APERTURE_REQUIRE_MPV_FUNCTION(mpv_render_context_create);
            APERTURE_REQUIRE_MPV_FUNCTION(mpv_render_context_set_update_callback);
            APERTURE_REQUIRE_MPV_FUNCTION(mpv_render_context_update);
            APERTURE_REQUIRE_MPV_FUNCTION(mpv_render_context_render);
            APERTURE_REQUIRE_MPV_FUNCTION(mpv_render_context_free);
        }
#undef APERTURE_REQUIRE_MPV_FUNCTION
        if (!missing.empty()) { Clear(); return false; }
        return true;
    }
};
#undef APERTURE_MPV_API_FUNCTIONS

class MpvApiOwner : protected MpvApiFunctions {
public:
    MpvApiOwner() = default;
    MpvApiOwner(const MpvApiOwner&) = delete;
    MpvApiOwner& operator=(const MpvApiOwner&) = delete;
    ~MpvApiOwner() { ResetMpvApi(); }
    const std::vector<MpvInitializationDiagnostic>& GetInitializationDiagnostics() const { return diagnostics; }
    void SetInitializationDiagnosticSink(std::function<void(const MpvInitializationDiagnostic&)> sink) {
        diagnosticSink = std::move(sink);
    }

protected:
    bool LoadMpvApi(MpvApiProfile profile, const char* owner, HMODULE hint = nullptr) {
        ResetMpvApi();
        diagnostics.clear();
        essentialOptionFailed = false;
        ownerName = owner;
        if (hint) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                    reinterpret_cast<LPCWSTR>(hint), &hMpvDll);
        if (!hMpvDll) hMpvDll = DllLoader::LoadPackageLocalDll(L"libmpv-2.dll");
        if (!hMpvDll) {
            RecordInitializationFailure("load package-local libmpv-2.dll (Win32 " +
                                        std::to_string(GetLastError()) + ")", 0);
            return false;
        }
        std::string missing;
        if (!Bind(hMpvDll, profile, missing)) {
            RecordInitializationFailure("missing required API: " + missing, 0);
            ResetMpvApi();
            return false;
        }
        return true;
    }

    // Call only after all event threads and render/client handles have stopped.
    void ResetMpvApi() {
        Clear();
        if (hMpvDll) FreeLibrary(hMpvDll);
        hMpvDll = nullptr;
    }

    void RecordInitializationFailure(const std::string& operation, int code,
                                     bool essential = true) {
        MpvInitializationDiagnostic diagnostic;
        diagnostic.message = ownerName + ": " + operation;
        diagnostic.errorCode = code;
        diagnostic.essential = essential;
        if (code < 0) {
            const char* detail = fn_mpv_error_string ? fn_mpv_error_string(code) : nullptr;
            diagnostic.message += detail ? std::string(": ") + detail : " (mpv error " + std::to_string(code) + ")";
        }
        diagnostics.push_back(diagnostic);
        OutputDebugStringW((Utf8ToUtf16(diagnostic.message) + L"\n").c_str());
        if (diagnosticSink) diagnosticSink(diagnostic);
    }

    bool SetMpvOption(mpv_handle* handle, const char* name, const char* value,
                      MpvOptionRequirement requirement = MpvOptionRequirement::Optional,
                      const char* fallback = nullptr) {
        const int result = fn_mpv_set_option_string ? fn_mpv_set_option_string(handle, name, value) : MPV_ERROR_UNSUPPORTED;
        if (result >= 0) return true;
        const bool essential = requirement == MpvOptionRequirement::Essential;
        RecordInitializationFailure(std::string("option ") + name + "=" + value, result, essential);
        if (fallback) {
            const int retry = fn_mpv_set_option_string ? fn_mpv_set_option_string(handle, name, fallback) : MPV_ERROR_UNSUPPORTED;
            if (retry >= 0) return true;
            RecordInitializationFailure(std::string("fallback ") + name + "=" + fallback, retry, essential);
        }
        essentialOptionFailed |= essential;
        return false;
    }

    bool SetMpvWindow(mpv_handle* handle, int64_t windowId) {
        const int result = fn_mpv_set_option ? fn_mpv_set_option(handle, "wid", MPV_FORMAT_INT64, &windowId) : MPV_ERROR_UNSUPPORTED;
        if (result >= 0) return true;
        essentialOptionFailed = true;
        RecordInitializationFailure("window attachment (wid)", result);
        return false;
    }
    bool EssentialOptionsAccepted() const { return !essentialOptionFailed; }
    HMODULE hMpvDll = nullptr;

private:
    bool essentialOptionFailed = false;
    std::string ownerName;
    std::vector<MpvInitializationDiagnostic> diagnostics;
    std::function<void(const MpvInitializationDiagnostic&)> diagnosticSink;
};

} // namespace Aperture
