// Feature gate: experience modes (Simple/Advanced/Custom) controlling visibility of
// Advanced-tier player features. Pure logic — UI layers consult these helpers.
#pragma once
#include <array>
#include <string>
#include <vector>

enum class ExperienceMode { Simple, Advanced, Custom };

namespace FeatureGate {

enum class FeatureId {
    AudioBalance, AudioChannelMap, AudioDelay, NightMode, AudioDevicePicker, ReplayGain, AudioPassthrough, PitchCorrection,
    SubtitleDelay, SubtitleSafeArea,
    SubtitleStyleProfiles, DualSubtitles, VideoAdjustments, Deinterlace, ABRepeat,
    PlaybackDiagnostics, OnlineSubtitles, SleepTimer, Shuffle, PlayAndExit,
    AlwaysOnTop, JumpToTime, MetadataEditor,
    ForcedSubsOnly, SubtitleCodepage, SubtitleFont, DualSubtitleStyle, SubtitleSync,
    CustomAspect, CustomRotation, ScreenshotFormat, JumpSize, LogViewer,
    ZoomPan,
    InternetRadio,
    Count
};

enum class FeatureTier { Simple, Advanced };

struct FeatureInfo {
    FeatureId id;
    const char* key;            // stable persisted id, e.g. "audio_balance"
    FeatureTier tier;
    const char* group;          // "audio" | "subtitles" | "video" | "playback"
    const char* displayNameKey; // localization key, e.g. "settings.experience.feature.audio_balance"
    std::vector<const char*> menuIds; // ContextMenuItem::id values this feature covers
    std::vector<const char*> menuIdPrefixes; // dynamic ids built as prefix + number (e.g. "sec_track_5")
};

// Catalog (ordered by group).
inline const std::array<FeatureInfo, static_cast<size_t>(FeatureId::Count)>& Catalog() {
    static const std::array<FeatureInfo, static_cast<size_t>(FeatureId::Count)> catalog = {{
        { FeatureId::AudioBalance, "audio_balance", FeatureTier::Advanced, "audio",
          "settings.experience.feature.audio_balance",
          { "audio_balance_menu", "abal_title", "abal_left_10", "abal_right_10", "abal_full_left", "abal_center", "abal_full_right" } },
        { FeatureId::AudioChannelMap, "audio_channel_map", FeatureTier::Advanced, "audio",
          "settings.experience.feature.audio_channel_map",
          { "audio_channel_menu", "ach_auto", "ach_stereo", "ach_mono_left", "ach_mono_right", "ach_swap" } },
        { FeatureId::AudioDelay, "audio_delay", FeatureTier::Advanced, "audio",
          "settings.experience.feature.audio_delay",
          { "adelay_title", "adelay_inc", "adelay_dec", "adelay_rst" } },
        { FeatureId::NightMode, "night_mode", FeatureTier::Advanced, "audio",
          "settings.experience.feature.night_mode",
          { "night_mode_menu", "night_off", "night_light", "night_medium", "night_strong" } },
        { FeatureId::AudioDevicePicker, "audio_device_picker", FeatureTier::Advanced, "audio",
          "settings.experience.feature.audio_device_picker",
          { "audio_device_menu", "adev_auto" },
          { "adev_" } },
        { FeatureId::ReplayGain, "replaygain", FeatureTier::Advanced, "audio",
          "settings.experience.feature.replaygain",
          { "replaygain_menu", "rg_title", "rg_off", "rg_track", "rg_album", "rg_pre_title" },
          { "rg_pre_" } },
        { FeatureId::AudioPassthrough, "audio_passthrough", FeatureTier::Advanced, "audio",
          "settings.experience.feature.audio_passthrough",
          { "audio_passthrough" } },
        { FeatureId::PitchCorrection, "pitch_correction", FeatureTier::Advanced, "audio",
          "settings.experience.feature.pitch_correction",
          { "pitch_correction" } },
        { FeatureId::InternetRadio, "internet_radio", FeatureTier::Simple, "audio",
          "settings.experience.feature.internet_radio",
          { "internet_radio" },
          { "radio_" } },
        { FeatureId::SubtitleDelay, "subtitle_delay", FeatureTier::Advanced, "subtitles",
          "settings.experience.feature.subtitle_delay",
          { "sdelay_title", "sdelay_inc", "sdelay_dec", "sdelay_rst" } },
        { FeatureId::SubtitleSafeArea, "subtitle_safe_area", FeatureTier::Advanced, "subtitles",
          "settings.experience.feature.subtitle_safe_area",
          { "sub_safe_area_menu", "preset_off", "preset_action", "preset_title", "preset_wide", "preset_cinema" } },
        { FeatureId::SubtitleStyleProfiles, "subtitle_style_profiles", FeatureTier::Advanced, "subtitles",
          "settings.experience.feature.subtitle_style_profiles",
          { "sub_style_profiles", "save_sub_prof", "restore_sub_def", "rename_prof_menu", "del_prof_menu" } },
        { FeatureId::DualSubtitles, "dual_subtitles", FeatureTier::Advanced, "subtitles",
          "settings.experience.feature.dual_subtitles",
          { "sec_hdr", "sec_off" },
          { "sec_track_" } },
        { FeatureId::OnlineSubtitles, "online_subtitles", FeatureTier::Simple, "subtitles",
          "settings.experience.feature.online_subtitles",
          { "sub_online" } },
        { FeatureId::ForcedSubsOnly, "forced_subs_only", FeatureTier::Simple, "subtitles",
          "settings.experience.feature.forced_subs_only",
          { "sub_forced_only" } },
        { FeatureId::SubtitleCodepage, "subtitle_codepage", FeatureTier::Advanced, "subtitles",
          "settings.experience.feature.subtitle_codepage",
          { "sub_charset_menu" },
          { "sub_cp_" } },
        { FeatureId::SubtitleFont, "subtitle_font", FeatureTier::Advanced, "subtitles",
          "settings.experience.feature.subtitle_font",
          { "sub_font_menu" },
          { "subfont_" } },
        { FeatureId::DualSubtitleStyle, "dual_subtitle_style", FeatureTier::Advanced, "subtitles",
          "settings.experience.feature.dual_subtitle_style",
          { "dual_style_menu" },
          { "dstyle_" } },
        { FeatureId::SubtitleSync, "subtitle_sync", FeatureTier::Advanced, "subtitles",
          "settings.experience.feature.subtitle_sync",
          { "subsync_audio", "subsync_sub", "subsync_clear" } },
        { FeatureId::VideoAdjustments, "video_adjustments", FeatureTier::Advanced, "video",
          "settings.experience.feature.video_adjustments",
          { "video_adjustments_menu", "vadjust_rot", "vadjust_bright", "vadjust_contrast", "vadjust_sat", "vadjust_gamma", "vadjust_hue",
            "adj_title", "adj_inc", "adj_dec" } },
        { FeatureId::CustomAspect, "custom_aspect", FeatureTier::Advanced, "video",
          "settings.experience.feature.custom_aspect",
          { "custom_aspect" } },
        { FeatureId::CustomRotation, "custom_rotation", FeatureTier::Advanced, "video",
          "settings.experience.feature.custom_rotation",
          { "rot_custom" } },
        { FeatureId::ScreenshotFormat, "screenshot_format", FeatureTier::Advanced, "video",
          "settings.experience.feature.screenshot_format",
          { "screenshot_format" } },
        { FeatureId::Deinterlace, "deinterlace", FeatureTier::Advanced, "video",
          "settings.experience.feature.deinterlace",
          { "deinterlace_menu", "di_off", "di_bwdif", "di_bwdif_field", "di_yadif" } },
        { FeatureId::ZoomPan, "zoom_pan", FeatureTier::Advanced, "video",
          "settings.experience.feature.zoom_pan",
          { "zoom_in", "zoom_out", "pan_reset" } },
        { FeatureId::ABRepeat, "ab_repeat", FeatureTier::Advanced, "playback",
          "settings.experience.feature.ab_repeat",
          { "ab_cycle" } },
        { FeatureId::PlaybackDiagnostics, "playback_diagnostics", FeatureTier::Advanced, "playback",
          "settings.experience.feature.playback_diagnostics",
          { "diagnostics" } },
        { FeatureId::SleepTimer, "sleep_timer", FeatureTier::Simple, "playback",
          "settings.experience.feature.sleep_timer",
          { "sleep_timer", "sleep_15", "sleep_30", "sleep_60", "sleep_90", "stop_after_current", "sleep_cancel" } },
        { FeatureId::Shuffle, "shuffle", FeatureTier::Simple, "playback",
          "settings.experience.feature.shuffle",
          { "shuffle" } },
        { FeatureId::PlayAndExit, "play_and_exit", FeatureTier::Simple, "playback",
          "settings.experience.feature.play_and_exit",
          { "exit_after_playback" } },
        { FeatureId::AlwaysOnTop, "always_on_top", FeatureTier::Simple, "playback",
          "settings.experience.feature.always_on_top",
          { "always_on_top" } },
        { FeatureId::JumpToTime, "jump_to_time", FeatureTier::Simple, "playback",
          "settings.experience.feature.jump_to_time",
          { "goto_time" } },
        { FeatureId::MetadataEditor, "metadata_editor", FeatureTier::Simple, "playback",
          "settings.experience.feature.metadata_editor",
          { "edit_metadata" } },
        { FeatureId::JumpSize, "jump_size", FeatureTier::Simple, "playback",
          "settings.experience.feature.jump_size",
          { "jump_size_menu" },
          { "jump_" } },
        { FeatureId::LogViewer, "log_viewer", FeatureTier::Advanced, "playback",
          "settings.experience.feature.log_viewer",
          { "open_log_viewer" } },
    }};
    return catalog;
}

inline const FeatureInfo* FindFeature(FeatureId id) {
    for (const auto& info : Catalog()) {
        if (info.id == id) return &info;
    }
    return nullptr;
}

inline ExperienceMode ModeFromString(const std::string& value) {
    if (value == "advanced") return ExperienceMode::Advanced;
    if (value == "custom") return ExperienceMode::Custom;
    return ExperienceMode::Simple; // Unknown values fall back to Simple.
}

inline const char* ModeToString(ExperienceMode mode) {
    switch (mode) {
        case ExperienceMode::Advanced: return "advanced";
        case ExperienceMode::Custom: return "custom";
        case ExperienceMode::Simple: break;
    }
    return "simple";
}

inline const char* NormalizeModeString(const std::string& value) {
    return ModeToString(ModeFromString(value));
}

// Feature lookup by persisted key; returns false when unknown.
inline bool TryFeatureByKey(const std::string& key, FeatureId& out) {
    for (const auto& info : Catalog()) {
        if (key == info.key) { out = info.id; return true; }
    }
    return false;
}

// Core resolution:
//   Simple  -> tier == Simple
//   Advanced-> always true
//   Custom  -> key present in customEnabled
inline bool FeatureEnabled(ExperienceMode mode, const std::vector<std::string>& customEnabled, FeatureId id) {
    const FeatureInfo* info = FindFeature(id);
    if (!info) return true;
    if (mode == ExperienceMode::Advanced) return true;
    if (mode == ExperienceMode::Simple) return info->tier == FeatureTier::Simple;
    for (const auto& key : customEnabled) {
        if (key == info->key) return true;
    }
    return false;
}

// Menu-item visibility: unknown ids are ALWAYS visible (default-open surface).
inline bool MenuItemVisible(ExperienceMode mode, const std::vector<std::string>& customEnabled, const std::string& menuItemId) {
    for (const auto& info : Catalog()) {
        for (const char* menuId : info.menuIds) {
            if (menuItemId == menuId) return FeatureEnabled(mode, customEnabled, info.id);
        }
    }
    // Dynamic ids: match by prefix (e.g. "sec_track_5"); only a real prefix counts.
    for (const auto& info : Catalog()) {
        for (const char* prefix : info.menuIdPrefixes) {
            if (menuItemId.rfind(prefix, 0) == 0) return FeatureEnabled(mode, customEnabled, info.id);
        }
    }
    return true;
}

// Sanitizer for persisted custom lists: keep only known feature keys, drop duplicates, preserve order.
inline std::vector<std::string> SanitizeFeatureKeys(const std::vector<std::string>& keys) {
    std::vector<std::string> clean;
    for (const auto& key : keys) {
        FeatureId id;
        if (!TryFeatureByKey(key, id)) continue;
        bool duplicate = false;
        for (const auto& existing : clean) {
            if (existing == key) { duplicate = true; break; }
        }
        if (!duplicate) clean.push_back(key);
    }
    return clean;
}

} // namespace FeatureGate
