#pragma once

#include "../support/TestFramework.h"
#include "../../src/MpvPlayer.h"
#include <cstring>
#include <type_traits>

namespace Aperture::Test {
namespace MpvInitializationFixture {
inline const char* missingExport = nullptr;
inline void DummyExport() {}
inline FARPROC WINAPI Resolve(HMODULE, LPCSTR name) {
    if (missingExport && std::strcmp(missingExport, name) == 0) return nullptr;
    return reinterpret_cast<FARPROC>(&DummyExport);
}
inline int SetString(mpv_handle*, const char*, const char* value) {
    return std::strcmp(value, "rejected") == 0 ? MPV_ERROR_OPTION_ERROR : 0;
}
inline int RejectWindow(mpv_handle*, const char*, mpv_format, void*) { return MPV_ERROR_OPTION_ERROR; }
class OptionHarness : public MpvApiOwner {
public:
    OptionHarness() { fn_mpv_set_option_string = &SetString; fn_mpv_set_option = &RejectWindow; }
    using MpvApiOwner::SetMpvOption;
    using MpvApiOwner::SetMpvWindow;
    using MpvApiOwner::EssentialOptionsAccepted;
};
}

TEST_CASE(MpvInitialization, MissingRequiredExportsClearTheWholeBindingTable) {
    using namespace MpvInitializationFixture;
    MpvApiFunctions api;
    std::string missing;
    for (const auto* name : { "mpv_create", "mpv_set_option_string", "mpv_get_property", "mpv_observe_property" }) {
        missingExport = name;
        TEST_ASSERT(!api.Bind(nullptr, MpvApiProfile::Playback, missing, &Resolve));
        TEST_ASSERT(missing.find(name) != std::string::npos);
        TEST_ASSERT(api.fn_mpv_create == nullptr);
        TEST_ASSERT(api.fn_mpv_render_context_free == nullptr);
    }
    missingExport = nullptr;
}

TEST_CASE(MpvInitialization, ProfilesRequireTheirOwnApisAndAllowWakeupFallback) {
    using namespace MpvInitializationFixture;
    MpvApiFunctions api;
    std::string missing;
    for (const auto* name : { "mpv_wakeup", "mpv_destroy", "mpv_command_string" }) {
        missingExport = name;
        TEST_ASSERT(api.Bind(nullptr, MpvApiProfile::Playback, missing, &Resolve));
        TEST_ASSERT(api.Bind(nullptr, MpvApiProfile::SoftwareRenderer, missing, &Resolve));
    }
    for (const auto* name : { "mpv_render_context_free", "mpv_render_context_update", "mpv_render_context_set_update_callback" }) {
        missingExport = name;
        TEST_ASSERT(api.Bind(nullptr, MpvApiProfile::Playback, missing, &Resolve));
        TEST_ASSERT(!api.Bind(nullptr, MpvApiProfile::SoftwareRenderer, missing, &Resolve));
    }
    missingExport = nullptr;
}

TEST_CASE(MpvInitialization, OptionalFailureKeepsInitializationUsableAndFallbackIsChecked) {
    using namespace MpvInitializationFixture;
    OptionHarness api;
    unsigned messages = 0;
    api.SetInitializationDiagnosticSink([&](const MpvInitializationDiagnostic& diagnostic) {
        ++messages; TEST_ASSERT(!diagnostic.essential);
    });
    TEST_ASSERT(!api.SetMpvOption(nullptr, "demuxer-max-bytes", "rejected"));
    TEST_ASSERT(api.EssentialOptionsAccepted());
    TEST_ASSERT(api.SetMpvOption(nullptr, "hwdec", "rejected", MpvOptionRequirement::Optional, "no"));
    TEST_ASSERT(!api.SetMpvOption(nullptr, "hwdec", "rejected", MpvOptionRequirement::Optional, "rejected"));
    TEST_ASSERT(api.EssentialOptionsAccepted());
    TEST_ASSERT_EQ(messages, 4u);
    TEST_ASSERT_EQ(api.GetInitializationDiagnostics().size(), size_t(4));
    TEST_ASSERT(api.GetInitializationDiagnostics()[0].message.find("demuxer-max-bytes") != std::string::npos);
    TEST_ASSERT_EQ(api.GetInitializationDiagnostics()[0].errorCode, int(MPV_ERROR_OPTION_ERROR));
}

TEST_CASE(MpvInitialization, EssentialOutputAndWindowRejectionsPreventInitialization) {
    using namespace MpvInitializationFixture;
    OptionHarness output;
    TEST_ASSERT(!output.SetMpvOption(nullptr, "vo", "rejected", MpvOptionRequirement::Essential));
    TEST_ASSERT(!output.EssentialOptionsAccepted());
    TEST_ASSERT(output.GetInitializationDiagnostics()[0].essential);
    OptionHarness window;
    TEST_ASSERT(!window.SetMpvWindow(nullptr, 42));
    TEST_ASSERT(!window.EssentialOptionsAccepted());
    TEST_ASSERT(window.GetInitializationDiagnostics()[0].message.find("wid") != std::string::npos);
    static_assert(!std::is_copy_constructible_v<MpvApiOwner>);
}

TEST_CASE(MpvInitialization, BundledProfilesHonorEssentialPolicyAndRetainIndependentDllReferences) {
    if (!FileExists(GetExeDir() + L"\\libmpv-2.dll")) {
        TEST_SKIP("Bundled libmpv-2.dll is absent from the isolated test directory");
    }
    struct Window {
        HWND parent = CreateWindowExW(0, L"STATIC", L"mpv profile test", WS_POPUP, 0, 0, 320, 180,
                                      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND child = CreateWindowExW(0, L"STATIC", L"mpv video", WS_CHILD, 0, 0, 320, 180,
                                     parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (parent) DestroyWindow(parent); }
    } window;
    TEST_ASSERT(window.parent && window.child);
    if (!window.parent || !window.child) return;
    auto checkDiagnostics = [](const auto& player) {
        for (const auto& diagnostic : player.GetInitializationDiagnostics()) std::cout << "  " << diagnostic.message << "\n";
        // Bundled builds omit ytdl; older engines can omit optional renderer optimizations.
        // All other rejections remain test failures.
        for (const auto& diagnostic : player.GetInitializationDiagnostics()) {
            TEST_ASSERT(!diagnostic.essential);
            TEST_ASSERT_EQ(diagnostic.errorCode, int(MPV_ERROR_OPTION_NOT_FOUND));
            TEST_ASSERT(diagnostic.message.find("option ytdl=no") != std::string::npos ||
                        diagnostic.message.find("option d3d11-gpu-timing=no") != std::string::npos ||
                        diagnostic.message.find("option scale-direct-nv12=yes") != std::string::npos);
        }
    };
    MpvPlayer main;
    const bool mainInitialized = main.Initialize(window.parent, window.child);
    TEST_ASSERT(mainInitialized);
    checkDiagnostics(main);
    if (!mainInitialized) return;
    MpvPreviewPlayer preview;
    TEST_ASSERT(preview.Initialize(nullptr, main.GetMpvDll()));
    checkDiagnostics(preview);
    main.Shutdown();
    TEST_ASSERT(main.GetMpvDll() == nullptr);
    // A real API call after the main owner released its DLL reference.
    preview.StopPreview();
    preview.Shutdown();
    MpvHubExtractor hub;
    // Loading a missing local target still initializes the actual software renderer.
    hub.Begin(GetExeDir() + L"\\missing-profile-fixture.mkv", 0);
    TEST_ASSERT(hub.IsInitialized());
    checkDiagnostics(hub);
    hub.Cancel();
    hub.Shutdown();
}

} // namespace Aperture::Test
