#pragma once

#include "../support/TestFramework.h"
#include "../../src/MpvPlayer.h"
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace Aperture {
// Exercise production track publication and diagnostics without a DLL or event thread.
struct MpvTrackTestAccess {
    MpvPlayer player;
    int64_t secondaryId = 0;
    std::vector<uint64_t> callbackVersions;
    std::function<void()> diagnosticReadHook;
    std::string diagnosticTransfer, diagnosticToneMapping, diagnosticHwDec;
    inline static constexpr const char* counterProperties[] = {"decoder-frame-drop-count", "frame-drop-count", "mistimed-frame-count"};
    int64_t counterValues[3]{};
    int counterResults[3]{MPV_ERROR_PROPERTY_NOT_FOUND, MPV_ERROR_PROPERTY_NOT_FOUND, MPV_ERROR_PROPERTY_NOT_FOUND};
    std::vector<std::string> counterReads, hwdecReads;
    MpvTrackTestAccess() {
        player.mpv = reinterpret_cast<mpv_handle*>(this);
        player.fn_mpv_get_property = &Property;
        player.fn_mpv_get_property_string = &StringProperty;
        player.fn_mpv_free = &std::free;
        player.SetOnTracksChanged([this] { callbackVersions.push_back(player.GetTracksVersion()); });
    }
    ~MpvTrackTestAccess() { player.ClearCallbacks(); player.mpv = nullptr; }
    void Refresh() { player.RefreshTracks(); }
    void SetColorPlan(const std::string& hdrMode, const std::string& toneMapping) {
        std::lock_guard<std::mutex> lock(player.stateMutex);
        player.lastColorPlan.normalizedHdrMode = hdrMode;
        player.lastColorPlan.normalizedToneMapping = toneMapping;
    }
    static std::string_view TrackField(const char* name, unsigned& index) {
        std::string_view path(name);
        constexpr std::string_view prefix = "track-list/";
        if (path.substr(0, prefix.size()) != prefix) return {};
        path.remove_prefix(prefix.size());
        if (path.size() < 3 || path[0] < '0' || path[0] > '3' || path[1] != '/') return {};
        index = static_cast<unsigned>(path[0] - '0');
        return path.substr(2);
    }
    static int Property(mpv_handle* handle, const char* name, mpv_format format, void* output) {
        auto& fixture = *reinterpret_cast<MpvTrackTestAccess*>(handle);
        if (format == MPV_FORMAT_INT64) {
            if (std::strstr(name, "frame")) fixture.counterReads.emplace_back(name);
            for (size_t i = 0; i < 3; ++i) {
                if (std::strcmp(name, counterProperties[i]) != 0) continue;
                *static_cast<int64_t*>(output) = fixture.counterValues[i];
                return fixture.counterResults[i];
            }
        }
        if (format == MPV_FORMAT_INT64 && std::strcmp(name, "secondary-sid") == 0) {
            *static_cast<int64_t*>(output) = fixture.secondaryId; return 0;
        }
        if (format == MPV_FORMAT_INT64 && std::strcmp(name, "track-list/count") == 0) {
            *static_cast<int64_t*>(output) = 4; return 0;
        }
        unsigned index = 0;
        const auto field = TrackField(name, index);
        if (format == MPV_FORMAT_INT64 && (field == "id" || field == "demux-channel-count")) {
            *static_cast<int64_t*>(output) = field == "id" ? (index == 0 ? 10 : index) : (index == 0 ? 2 : 0);
            return 0;
        }
        if (format == MPV_FORMAT_FLAG && (field == "selected" || field == "default" || field == "external" || field == "forced")) {
            *static_cast<int*>(output) = (field == "selected" || field == "default") && index <= 1;
            return 0;
        }
        return MPV_ERROR_PROPERTY_NOT_FOUND;
    }
    static char* StringProperty(mpv_handle* handle, const char* name) {
        auto& fixture = *reinterpret_cast<MpvTrackTestAccess*>(handle);
        if (std::strncmp(name, "hwdec-", 6) == 0) fixture.hwdecReads.emplace_back(name);
        if (std::strcmp(name, "hwdec-current") == 0 && fixture.diagnosticReadHook)
            fixture.diagnosticReadHook();
        unsigned index = 0;
        const auto field = TrackField(name, index);
        const char* value = nullptr;
        if (std::strcmp(name, "hwdec-current") == 0 && !fixture.diagnosticHwDec.empty())
            value = fixture.diagnosticHwDec.c_str();
        else if (std::strcmp(name, "video-params/gamma") == 0 && !fixture.diagnosticTransfer.empty())
            value = fixture.diagnosticTransfer.c_str();
        else if (std::strcmp(name, "tone-mapping") == 0 && !fixture.diagnosticToneMapping.empty())
            value = fixture.diagnosticToneMapping.c_str();
        else if (field == "type") value = index == 0 ? "audio" : "sub";
        else if (field == "title") {
            constexpr const char* titles[] = { "Audio", "Primary", "Secondary two", "Secondary three" };
            value = titles[index];
        } else if (field == "lang") value = "eng";
        else if (field == "codec" || field == "codec-desc") value = index == 0 ? "AAC" : "SRT";
        if (!value) return nullptr;
        const size_t bytes = std::strlen(value) + 1;
        auto* copy = static_cast<char*>(std::malloc(bytes));
        if (copy) std::memcpy(copy, value, bytes);
        return copy;
    }
};
}

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

TEST_CASE(MpvInitialization, SecondarySubtitleSelectionPublishesTrackVersion) {
    MpvTrackTestAccess fixture;
    fixture.Refresh();
    TEST_ASSERT_EQ(fixture.player.GetTracksVersion(), uint64_t{1});
    TEST_ASSERT_EQ(fixture.callbackVersions.size(), size_t{1});
    const auto original = fixture.player.GetSubtitleTracks();
    TEST_ASSERT_EQ(original.size(), size_t{3});
    uint64_t cachedVersion = fixture.player.GetTracksVersion();
    auto cached = original;
    uint64_t expectedVersion = cachedVersion;
    int64_t previousSecondary = 0;
    for (const int64_t secondary : { 2, 2, 3, 0, 0 }) {
        fixture.secondaryId = secondary;
        fixture.Refresh();
        if (secondary != previousSecondary) ++expectedVersion;
        TEST_ASSERT_EQ(fixture.player.GetTracksVersion(), expectedVersion);
        TEST_ASSERT_EQ(fixture.callbackVersions.size(), static_cast<size_t>(expectedVersion));
        TEST_ASSERT_EQ(fixture.callbackVersions.back(), expectedVersion);
        TEST_ASSERT_EQ(fixture.player.GetCurrentSubtitleTrackId(), 1);
        TEST_ASSERT_EQ(fixture.player.GetCurrentSecondarySubtitleTrackId(), static_cast<int>(secondary));
        if (fixture.player.GetTracksVersion() != cachedVersion) {
            cached = fixture.player.GetSubtitleTracks();
            cachedVersion = fixture.player.GetTracksVersion();
        }
        for (size_t i = 0; i < cached.size(); ++i) {
            TEST_ASSERT_EQ(cached[i].id, static_cast<int>(i + 1));
            TEST_ASSERT_EQ(cached[i].isSelected, i == 0);
            TEST_ASSERT_EQ(cached[i].isSecondarySelected, secondary == static_cast<int64_t>(i + 1));
            TEST_ASSERT(!original[i].isSecondarySelected);
        }
        const auto audio = fixture.player.GetAudioTracks();
        TEST_ASSERT_EQ(audio.size(), size_t{1});
        TEST_ASSERT_EQ(audio[0].id, 10);
        TEST_ASSERT(audio[0].isSelected && !audio[0].isSecondarySelected);
        previousSecondary = secondary;
    }
}

TEST_CASE(MpvInitialization, DiagnosticsSnapshotsColorPlanBeforePropertyReads) {
    MpvTrackTestAccess fixture;
    fixture.diagnosticTransfer = "pq";
    struct Case {
        const char* initialMode;
        const char* updatedMode;
        const char* rawToneMapping;
        const wchar_t* expectedOutput;
    };
    for (const auto& test : {
        Case{"passthrough", "off", "", L"HDR Passthrough"},
        Case{"tone-map", "tone-map", "", L"SDR Tone-Mapped (hable)"},
        Case{"tone-map", "off", "mobius", L"SDR Tone-Mapped (mobius)"}
    }) {
        fixture.SetColorPlan(test.initialMode, "hable");
        fixture.diagnosticToneMapping = test.rawToneMapping;
        unsigned updates = 0;
        // Deterministically publish a newer plan after the state snapshot, during
        // an mpv read. Taking the state lock here also checks it is released.
        fixture.diagnosticReadHook = [&] {
            ++updates;
            fixture.SetColorPlan(test.updatedMode, "reinhard");
        };
        const auto diagnostics = fixture.player.GetDiagnostics();
        TEST_ASSERT_EQ(updates, 1u);
        TEST_ASSERT(diagnostics.isHdrSource);
        TEST_ASSERT_EQ(diagnostics.colorTransfer, std::wstring(L"pq"));
        TEST_ASSERT_EQ(diagnostics.activeOutputMode, std::wstring(test.expectedOutput));
        TEST_ASSERT_EQ(diagnostics.activeToneMapping, Utf8ToUtf16(test.rawToneMapping));
    }
    fixture.diagnosticReadHook = {};
    TEST_ASSERT_EQ(fixture.player.GetDiagnostics().activeOutputMode, std::wstring(L"SDR Clipped"));
}

TEST_CASE(MpvInitialization, DiagnosticsDistinguishesUnavailableFrameCountersFromZero) {
    MpvTrackTestAccess fixture;
    struct Case { int results[3]; int64_t values[3], expected[3]; };
    for (const auto& test : {
        Case{{MPV_ERROR_PROPERTY_NOT_FOUND, MPV_ERROR_PROPERTY_NOT_FOUND, MPV_ERROR_PROPERTY_NOT_FOUND}, {99, 99, 99}, {-1, -1, -1}},
        Case{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
        Case{{0, 0, 0}, {2, 3, 5}, {2, 3, 5}},
        Case{{0, 0, MPV_ERROR_PROPERTY_UNAVAILABLE}, {0, 11, 99}, {0, 11, -1}},
        Case{{MPV_ERROR_PROPERTY_UNAVAILABLE, MPV_ERROR_PROPERTY_NOT_FOUND, MPV_ERROR_PROPERTY_UNAVAILABLE}, {99, 99, 99}, {-1, -1, -1}}
    }) {
        for (size_t i = 0; i < 3; ++i) {
            fixture.counterResults[i] = test.results[i];
            fixture.counterValues[i] = test.values[i];
        }
        fixture.counterReads.clear();
        const auto diagnostics = fixture.player.GetDiagnostics();
        const int64_t actual[] = {diagnostics.droppedFrames, diagnostics.voDroppedFrames, diagnostics.mistimedFrames};
        TEST_ASSERT_EQ(fixture.counterReads.size(), size_t{3});
        for (size_t i = 0; i < 3; ++i) {
            TEST_ASSERT_EQ(actual[i], test.expected[i]);
            if (i < fixture.counterReads.size()) TEST_ASSERT_EQ(fixture.counterReads[i], std::string(MpvTrackTestAccess::counterProperties[i]));
        }
    }
}

TEST_CASE(MpvInitialization, DiagnosticsReportsEffectiveHardwareDecoderAvailability) {
    MpvTrackTestAccess fixture;
    for (const auto* value : {"", "d3d11va", "d3d11va-copy", "no", ""}) {
        fixture.diagnosticHwDec = value;
        fixture.hwdecReads.clear();
        TEST_ASSERT_EQ(fixture.player.GetDiagnostics().hwDec, Utf8ToUtf16(value));
        TEST_ASSERT_EQ(fixture.hwdecReads.size(), size_t{1});
        if (!fixture.hwdecReads.empty()) TEST_ASSERT_EQ(fixture.hwdecReads.front(), std::string("hwdec-current"));
    }
}

TEST_CASE(MpvInitialization, ExportPolicyAndBundledLifecycle) {
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

    OptionHarness optApi;
    unsigned messages = 0;
    optApi.SetInitializationDiagnosticSink([&](const MpvInitializationDiagnostic& diagnostic) {
        ++messages;
        TEST_ASSERT(!diagnostic.essential);
    });
    TEST_ASSERT(!optApi.SetMpvOption(nullptr, "demuxer-max-bytes", "rejected"));
    TEST_ASSERT(optApi.EssentialOptionsAccepted());
    TEST_ASSERT(optApi.SetMpvOption(nullptr, "hwdec", "rejected", MpvOptionRequirement::Optional, "no"));
    TEST_ASSERT(!optApi.SetMpvOption(nullptr, "hwdec", "rejected", MpvOptionRequirement::Optional, "rejected"));
    TEST_ASSERT(optApi.EssentialOptionsAccepted());
    TEST_ASSERT_EQ(messages, 4u);

    OptionHarness output;
    TEST_ASSERT(!output.SetMpvOption(nullptr, "vo", "rejected", MpvOptionRequirement::Essential));
    TEST_ASSERT(!output.EssentialOptionsAccepted());
    TEST_ASSERT(output.GetInitializationDiagnostics()[0].essential);
    OptionHarness windowOpt;
    TEST_ASSERT(!windowOpt.SetMpvWindow(nullptr, 42));
    TEST_ASSERT(!windowOpt.EssentialOptionsAccepted());
    static_assert(!std::is_copy_constructible_v<MpvApiOwner>);

    if (!FileExists(GetExeDir() + L"\\libmpv-2.dll")) {
        return;
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
    preview.StopPreview();
    preview.Shutdown();
    MpvHubExtractor hub;
    hub.Begin(GetExeDir() + L"\\missing-profile-fixture.mkv", 0);
    TEST_ASSERT(hub.IsInitialized());
    checkDiagnostics(hub);
    hub.Cancel();
    hub.Shutdown();
}

} // namespace Aperture::Test
