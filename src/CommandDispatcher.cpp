#include "CommandDispatcher.h"
#include "MpvPlayer.h"
#include "MediaLibrary.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string_view>
#include <unordered_map>

namespace Aperture {

namespace {

// Keep persistence and private-session isolation identical across commands.
template <class Update> void UpdateCurrentMediaPreferences(ICommandContext &context, Update update) {
    auto& session = context.GetPlaybackSession();
    if (!session.IsOpen() || session.GetCurrentFilePath().empty() || session.IsPrivateMode()) return;
    auto* item = context.GetLibrary().FindByPath(session.GetCurrentFilePath());
    if (!item) return;
    if (!item->preferences) item->preferences.emplace();
    update(*item->preferences);
    session.SetActivePreferences(item->preferences);
    context.SaveLibrary();
}

enum class ImageAdjustmentOperation { Increase, Decrease, Reset, Set };

struct ImageAdjustment {
    int (MpvPlayer::*get)() const;
    void (MpvPlayer::*set)(int);
    int Settings::*setting;
    std::optional<int> MediaPlaybackPreferences::*preference;
    const wchar_t* label;
};

bool ApplyImageAdjustment(ICommandContext& context, const CommandArgs& args,
                          const ImageAdjustment& adjustment, ImageAdjustmentOperation operation) {
    auto& player = context.GetPlayer();
    auto& settings = context.GetSettings();
    int value = 0;
    if (operation == ImageAdjustmentOperation::Set) {
        value = std::clamp(args.intVal, -100, 100);
    } else if (operation != ImageAdjustmentOperation::Reset) {
        const int current = (player.*adjustment.get)();
        const int step = args.intVal != 0 ? args.intVal : 5;
        value = std::clamp(operation == ImageAdjustmentOperation::Increase ? current + step : current - step, -100, 100);
    }
    (player.*adjustment.set)(value);
    settings.*adjustment.setting = value;
    context.SaveSettings();
    UpdateCurrentMediaPreferences(context, [&](MediaPlaybackPreferences& prefs) { prefs.*adjustment.preference = value; });
    wchar_t message[64];
    swprintf_s(message, operation == ImageAdjustmentOperation::Reset ? L"%ls: %d" : L"%ls: %+d", adjustment.label, value);
    context.ShowOSD(IconType::SettingsGear, message);
    return true;
}

constexpr ImageAdjustment kBrightness{&MpvPlayer::GetBrightness, &MpvPlayer::SetBrightness, &Settings::brightness,
                                      &MediaPlaybackPreferences::brightness, L"Brightness"};
constexpr ImageAdjustment kContrast{&MpvPlayer::GetContrast, &MpvPlayer::SetContrast, &Settings::contrast,
                                    &MediaPlaybackPreferences::contrast, L"Contrast"};
constexpr ImageAdjustment kSaturation{&MpvPlayer::GetSaturation, &MpvPlayer::SetSaturation, &Settings::saturation,
                                      &MediaPlaybackPreferences::saturation, L"Saturation"};
constexpr ImageAdjustment kGamma{&MpvPlayer::GetGamma, &MpvPlayer::SetGamma, &Settings::gamma, &MediaPlaybackPreferences::gamma, L"Gamma"};
constexpr ImageAdjustment kHue{&MpvPlayer::GetHue, &MpvPlayer::SetHue, &Settings::hue, &MediaPlaybackPreferences::hue, L"Hue"};

struct CommandDescriptor { AppCommand command; const char* id; const wchar_t* displayName; };

constexpr CommandDescriptor kCommands[] = {
    {AppCommand::None, "", L""}, {AppCommand::TogglePlay, "togglePlay", L"Play / Pause"}, {AppCommand::Play, "play", L""},
    {AppCommand::Pause, "pause", L""}, {AppCommand::SeekForward, "seekForward", L"Seek Forward"},
    {AppCommand::SeekBackward, "seekBackward", L"Seek Backward"}, {AppCommand::SeekForwardLarge, "seekForwardLarge", L"Seek Forward 10s"},
    {AppCommand::SeekBackwardLarge, "seekBackwardLarge", L"Seek Backward 10s"}, {AppCommand::SeekTo, "seekTo", L""},
    {AppCommand::VolumeUp, "volumeUp", L"Volume Up"}, {AppCommand::VolumeDown, "volumeDown", L"Volume Down"},
    {AppCommand::SetVolume, "setVolume", L""}, {AppCommand::ToggleMute, "toggleMute", L"Mute / Unmute"},
    {AppCommand::SetMuted, "setMuted", L""}, {AppCommand::ToggleFullscreen, "toggleFullscreen", L"Fullscreen"},
    {AppCommand::TogglePiP, "togglePiP", L"Picture-in-Picture"}, {AppCommand::CycleSubtitles, "cycleSubtitles", L"Cycle Subtitles"},
    {AppCommand::SelectSubtitleTrack, "selectSubtitleTrack", L""}, {AppCommand::ToggleSubtitles, "toggleSubtitles", L""},
    {AppCommand::CycleAudio, "cycleAudio", L"Cycle Audio"}, {AppCommand::SelectAudioTrack, "selectAudioTrack", L""},
    {AppCommand::RestartVideo, "restartVideo", L"Restart Video"}, {AppCommand::PlayNext, "playNext", L"Next"},
    {AppCommand::PlayPrevious, "playPrevious", L"Previous"}, {AppCommand::NextChapter, "nextChapter", L"Next Chapter"},
    {AppCommand::PreviousChapter, "previousChapter", L"Previous Chapter"},
    {AppCommand::CycleAspectRatio, "cycleAspectRatio", L"Aspect Ratio"}, {AppCommand::SetAspectRatio, "setAspectRatio", L""},
    {AppCommand::RotateVideo, "rotateVideo", L"Rotate Video (90°)"}, {AppCommand::SetVideoRotate, "setVideoRotate", L"Set Video Rotation"},
    {AppCommand::BrightnessPlus, "brightnessPlus", L"Brightness +5"}, {AppCommand::BrightnessMinus, "brightnessMinus", L"Brightness -5"},
    {AppCommand::BrightnessReset, "brightnessReset", L"Reset Brightness"}, {AppCommand::SetBrightness, "setBrightness", L"Set Brightness"},
    {AppCommand::ContrastPlus, "contrastPlus", L"Contrast +5"}, {AppCommand::ContrastMinus, "contrastMinus", L"Contrast -5"},
    {AppCommand::ContrastReset, "contrastReset", L"Reset Contrast"}, {AppCommand::SetContrast, "setContrast", L"Set Contrast"},
    {AppCommand::SaturationPlus, "saturationPlus", L"Saturation +5"}, {AppCommand::SaturationMinus, "saturationMinus", L"Saturation -5"},
    {AppCommand::SaturationReset, "saturationReset", L"Reset Saturation"}, {AppCommand::SetSaturation, "setSaturation", L"Set Saturation"},
    {AppCommand::GammaPlus, "gammaPlus", L"Gamma +5"}, {AppCommand::GammaMinus, "gammaMinus", L"Gamma -5"},
    {AppCommand::GammaReset, "gammaReset", L"Reset Gamma"}, {AppCommand::SetGamma, "setGamma", L"Set Gamma"},
    {AppCommand::HuePlus, "huePlus", L"Hue +5"}, {AppCommand::HueMinus, "hueMinus", L"Hue -5"},
    {AppCommand::HueReset, "hueReset", L"Reset Hue"}, {AppCommand::SetHue, "setHue", L"Set Hue"},
    {AppCommand::ResetVideoAdjustments, "resetVideoAdjustments", L"Reset Video Adjustments"},
    {AppCommand::AudioDelayPlus, "audioDelayPlus", L"Audio Delay +50ms"},
    {AppCommand::AudioDelayMinus, "audioDelayMinus", L"Audio Delay -50ms"},
    {AppCommand::AudioDelayReset, "audioDelayReset", L"Reset Audio Delay"}, {AppCommand::SetAudioDelay, "setAudioDelay", L""},
    {AppCommand::AudioBalanceLeft, "audioBalanceLeft", L"Audio Balance Left"},
    {AppCommand::AudioBalanceRight, "audioBalanceRight", L"Audio Balance Right"},
    {AppCommand::AudioBalanceReset, "audioBalanceReset", L"Reset Audio Balance"}, {AppCommand::SetAudioBalance, "setAudioBalance", L""},
    {AppCommand::CycleAudioChannelMap, "cycleAudioChannelMap", L"Cycle Channel Mapping"},
    {AppCommand::SetAudioChannelMap, "setAudioChannelMap", L""},
    {AppCommand::AudioChannelMapReset, "audioChannelMapReset", L"Reset Channel Mapping"},
    {AppCommand::ResetAudioBalanceAndChannelMap, "resetAudioBalanceAndChannelMap", L"Reset Audio Balance & Channel Map"},
    {AppCommand::SubDelayPlus, "subDelayPlus", L"Subtitle Delay +100ms"},
    {AppCommand::SubDelayMinus, "subDelayMinus", L"Subtitle Delay -100ms"},
    {AppCommand::SubDelayReset, "subDelayReset", L"Reset Subtitle Delay"}, {AppCommand::SetSubDelay, "setSubDelay", L""},
    {AppCommand::ClosePlayer, "closePlayer", L"Close Player"}, {AppCommand::FrameStepForward, "frameStepForward", L"Frame Step Forward"},
    {AppCommand::FrameStepBackward, "frameStepBackward", L"Frame Step Backward"}, {AppCommand::SpeedUp, "speedUp", L"Speed Up"},
    {AppCommand::SpeedDown, "speedDown", L"Speed Down"}, {AppCommand::SpeedReset, "speedReset", L"Reset Speed"},
    {AppCommand::SetSpeed, "setSpeed", L""}, {AppCommand::ToggleLoop, "toggleLoop", L"Toggle Loop"},
    {AppCommand::ToggleAutoplay, "toggleAutoplay", L"Toggle Autoplay"}, {AppCommand::OpenFile, "openFile", L""},
    {AppCommand::OpenSubtitleFile, "openSubtitleFile", L""},
    {AppCommand::SearchOnlineSubtitles, "searchOnlineSubtitles", L"Search Online Subtitles"},
    {AppCommand::ShowInExplorer, "showInExplorer", L""},
    {AppCommand::TogglePlaybackDiagnostics, "togglePlaybackDiagnostics", L"Toggle Diagnostics"},
    {AppCommand::TakeScreenshot, "takeScreenshot", L"Take Screenshot"},
    {AppCommand::TogglePrivateMode, "togglePrivateMode", L"Toggle Private Session"}, };

// Direct indexing requires descriptor order to match the enum.
static_assert([] {
    for (size_t i = 0; i < std::size(kCommands); ++i)
        if (static_cast<size_t>(kCommands[i].command) != i) return false;
    return std::size(kCommands) == static_cast<size_t>(AppCommand::TogglePrivateMode) + 1;
}());

const CommandDescriptor& DescribeCommand(AppCommand command) {
    const auto index = static_cast<size_t>(command);
    return kCommands[index < std::size(kCommands) ? index : 0];
}

} // namespace

CommandDispatcher::CommandDispatcher(ICommandContext* ctx) : context(ctx) {}

AppCommand CommandDispatcher::CommandIdFromString(const std::string& actionId) {
    static const auto commands = [] {
        std::unordered_map<std::string_view, AppCommand> result;
        result.reserve(std::size(kCommands) + 12);
        for (const auto& command : kCommands) result.emplace(command.id, command.command);
        result.emplace("prevChapter", AppCommand::PreviousChapter);
        result.emplace("rotate", AppCommand::RotateVideo);
        result.emplace("setRotation", AppCommand::SetVideoRotate);
        result.emplace("resetAdjustments", AppCommand::ResetVideoAdjustments);
        result.emplace("onlineSubtitles", AppCommand::SearchOnlineSubtitles);
        result.emplace("toggleDiagnostics", AppCommand::TogglePlaybackDiagnostics);
        result.emplace("toggleStats", AppCommand::TogglePlaybackDiagnostics);
        result.emplace("screenshot", AppCommand::TakeScreenshot);
        result.emplace("captureScreenshot", AppCommand::TakeScreenshot);
        result.emplace("togglePrivate", AppCommand::TogglePrivateMode);
        result.emplace("privateMode", AppCommand::TogglePrivateMode);
        result.emplace("togglePrivateSession", AppCommand::TogglePrivateMode);
        return result;
    }();
    const auto found = commands.find(actionId);
    return found != commands.end() ? found->second : AppCommand::None;
}

std::string CommandDispatcher::StringFromCommandId(AppCommand cmd) { return DescribeCommand(cmd).id; }

AppCommand CommandDispatcher::TranslateKeyChord(UINT vkCode, bool isCtrl, bool isShift, bool isAlt, bool isPlayerOpen, const Settings& settings) const {
    if (isPlayerOpen) {
        // 1. Check custom shortcut bindings in Settings
        for (const auto& pair : settings.shortcuts) {
            const auto& b = pair.second;
            if (b.vkCode == vkCode && b.ctrl == isCtrl && b.shift == isShift && b.alt == isAlt) {
                AppCommand cmd = CommandIdFromString(pair.first);
                if (cmd != AppCommand::None) return cmd;
            }
        }
        // Precise frame-by-frame seek with Ctrl+Left / Ctrl+Right fallback
        if (isCtrl && !isShift && !isAlt) {
            if (vkCode == 'S') return AppCommand::TakeScreenshot;
            if (vkCode == VK_LEFT) return AppCommand::FrameStepBackward;
            if (vkCode == VK_RIGHT) return AppCommand::FrameStepForward;
            if (vkCode == VK_NEXT) return AppCommand::NextChapter;
            if (vkCode == VK_PRIOR) return AppCommand::PreviousChapter;
        }
        if (!isCtrl && !isAlt && isShift) {
            if (vkCode == 'N') return AppCommand::NextChapter;
            if (vkCode == 'P') return AppCommand::PreviousChapter;
        }
        if (!isCtrl && !isAlt && !isShift) {
            if (vkCode == VK_SPACE || vkCode == 'K') return AppCommand::TogglePlay;
            if (vkCode == VK_LEFT) return AppCommand::SeekBackward;
            if (vkCode == VK_RIGHT) return AppCommand::SeekForward;
            if (vkCode == 'J') return AppCommand::SeekBackwardLarge;
            if (vkCode == 'L') return AppCommand::SeekForwardLarge;
            if (vkCode == VK_UP) return AppCommand::VolumeUp;
            if (vkCode == VK_DOWN) return AppCommand::VolumeDown;
            if (vkCode == 'M') return AppCommand::ToggleMute;
            if (vkCode == 'F') return AppCommand::ToggleFullscreen;
            if (vkCode == 'P') return AppCommand::TogglePiP;
            if (vkCode == 'S' || vkCode == 'C' || vkCode == 'V') return AppCommand::CycleSubtitles;
            if (vkCode == 'A' || vkCode == 'B') return AppCommand::CycleAudio;
            if (vkCode == 'R') return AppCommand::RestartVideo;
            if (vkCode == 'N') return AppCommand::PlayNext;
            if (vkCode == VK_NEXT) return AppCommand::NextChapter;
            if (vkCode == VK_PRIOR) return AppCommand::PreviousChapter;
            if (vkCode == VK_OEM_PERIOD) return AppCommand::FrameStepForward;
            if (vkCode == VK_OEM_COMMA) return AppCommand::FrameStepBackward;
            if (vkCode == VK_OEM_6) return AppCommand::SpeedUp;
            if (vkCode == VK_OEM_4) return AppCommand::SpeedDown;
            if (vkCode == VK_BACK) return AppCommand::SpeedReset;
            if (vkCode == 'I' || vkCode == 'D') return AppCommand::TogglePlaybackDiagnostics;
            if (vkCode == VK_ESCAPE) return AppCommand::ClosePlayer;
        }
    }
    return AppCommand::None;
}

bool CommandDispatcher::Execute(const std::string& actionId, const CommandArgs& args) {
    AppCommand cmd = CommandIdFromString(actionId);
    if (cmd != AppCommand::None) { return Execute(cmd, args); }
    return false;
}

bool CommandDispatcher::Execute(AppCommand cmd, const CommandArgs& args) {
    if (!context) return false;
    Settings& settings = context->GetSettings();
    MpvPlayer& player = context->GetPlayer();
    PlaybackSession& session = context->GetPlaybackSession();
    switch (cmd) {
    case AppCommand::TogglePlay: {
        if (session.IsOpen()) {
            bool wasPlaying = player.IsPlaying();
            player.TogglePlay();
            context->TriggerPlayPauseBadge(!wasPlaying);
            session.SetPlayState(!wasPlaying);
            return true;
        }
        return false;
    }
    case AppCommand::Play: {
        if (session.IsOpen()) { player.Play(); context->TriggerPlayPauseBadge(true); session.SetPlayState(true); return true; }
        return false;
    }
    case AppCommand::Pause: {
        if (session.IsOpen()) { player.Pause(); context->TriggerPlayPauseBadge(false); session.SetPlayState(false); return true; }
        return false;
    }
    case AppCommand::SeekForward: {
        if (session.IsOpen()) {
            player.SeekRelative(settings.arrowSeekStepSec);
            context->TriggerDoubleTapFeedback(true, (int)settings.arrowSeekStepSec);
            return true;
        }
        return false;
    }
    case AppCommand::SeekBackward: {
        if (session.IsOpen()) {
            player.SeekRelative(-settings.arrowSeekStepSec);
            context->TriggerDoubleTapFeedback(false, (int)settings.arrowSeekStepSec);
            return true;
        }
        return false;
    }
    case AppCommand::SeekForwardLarge: {
        if (session.IsOpen()) { player.SeekRelative(10.0); context->TriggerDoubleTapFeedback(true, 10); return true; }
        return false;
    }
    case AppCommand::SeekBackwardLarge: {
        if (session.IsOpen()) { player.SeekRelative(-10.0); context->TriggerDoubleTapFeedback(false, 10); return true; }
        return false;
    }
    case AppCommand::SeekTo: {
        if (session.IsOpen()) { double targetPos = std::max(0.0, args.doubleVal); player.Seek(targetPos); return true; }
        return false;
    }
    case AppCommand::VolumeUp: {
        double newVol = std::min(1.0, player.GetVolume() + 0.05);
        player.SetVolume(newVol);
        player.SetMuted(false);
        settings.volume = newVol;
        settings.muted = false;
        if (newVol > 0.01) session.SetPriorUnmutedVolume(newVol);
        int pct = (int)std::round(newVol * 100.0);
        context->ShowOSD(newVol <= 0.01 ? IconType::VolumeMuted : (newVol > 0.5 ? IconType::VolumeHigh : IconType::VolumeLow), std::to_wstring(pct) + L"%");
        return true;
    }
    case AppCommand::VolumeDown: {
        double newVol = std::max(0.0, player.GetVolume() - 0.05);
        player.SetVolume(newVol);
        settings.volume = newVol;
        if (newVol > 0.01) session.SetPriorUnmutedVolume(newVol);
        int pct = (int)std::round(newVol * 100.0);
        context->ShowOSD(newVol <= 0.01 ? IconType::VolumeMuted : (newVol > 0.5 ? IconType::VolumeHigh : IconType::VolumeLow), std::to_wstring(pct) + L"%");
        return true;
    }
    case AppCommand::SetVolume: {
        double vol = std::clamp(args.doubleVal, 0.0, 1.0);
        player.SetVolume(vol);
        settings.volume = vol;
        if (vol > 0.01) { settings.muted = false; player.SetMuted(false); session.SetPriorUnmutedVolume(vol); }
        int pct = (int)std::round(vol * 100.0);
        context->ShowOSD(vol <= 0.01 ? IconType::VolumeMuted : (vol > 0.5 ? IconType::VolumeHigh : IconType::VolumeLow), std::to_wstring(pct) + L"%");
        return true;
    }
    case AppCommand::ToggleMute: {
        bool currentlyMuted = player.IsMuted() || player.GetVolume() <= 0.01;
        if (currentlyMuted) {
            player.SetMuted(false);
            settings.muted = false;
            double restoreVol = player.GetVolume();
            if (restoreVol <= 0.01) {
                restoreVol = session.GetPriorUnmutedVolume() > 0.01 ? session.GetPriorUnmutedVolume() : 1.0;
                player.SetVolume(restoreVol);
                settings.volume = restoreVol;
            }
            int pct = (int)std::round(restoreVol * 100.0);
            context->ShowOSD(restoreVol > 0.5 ? IconType::VolumeHigh : IconType::VolumeLow, L"Unmuted (" + std::to_wstring(pct) + L"%)");
        } else {
            if (player.GetVolume() > 0.01) session.SetPriorUnmutedVolume(player.GetVolume());
            player.SetMuted(true);
            settings.muted = true;
            context->ShowOSD(IconType::VolumeMuted, L"Muted");
        }
        return true;
    }
    case AppCommand::SetMuted: {
        bool mute = (args.intVal != 0);
        if (!mute) {
            player.SetMuted(false);
            settings.muted = false;
            double restoreVol = player.GetVolume();
            if (restoreVol <= 0.01) {
                restoreVol = session.GetPriorUnmutedVolume() > 0.01 ? session.GetPriorUnmutedVolume() : 1.0;
                player.SetVolume(restoreVol);
                settings.volume = restoreVol;
            }
            int pct = (int)std::round(restoreVol * 100.0);
            context->ShowOSD(restoreVol > 0.5 ? IconType::VolumeHigh : IconType::VolumeLow, L"Unmuted (" + std::to_wstring(pct) + L"%)");
        } else {
            if (player.GetVolume() > 0.01) session.SetPriorUnmutedVolume(player.GetVolume());
            player.SetMuted(true);
            settings.muted = true;
            context->ShowOSD(IconType::VolumeMuted, L"Muted");
        }
        return true;
    }
    case AppCommand::ToggleFullscreen: { context->ToggleFullscreen(); return true; }
    case AppCommand::TogglePiP: { context->TogglePiP(); return true; }
    case AppCommand::CycleSubtitles: {
        if (session.IsOpen()) {
            auto tracksBefore = player.GetSubtitleTracks();
            if (tracksBefore.empty()) { context->ShowOSD(IconType::Subtitles, L"No Subtitles Available"); } else {
                player.CycleSubtitleTrack();
                int currentId = player.GetCurrentSubtitleTrackId();
                std::wstring currentTitle = player.GetCurrentSubtitleTrackTitle();
                std::string currentLang;
                auto tracksAfter = player.GetSubtitleTracks();
                for (const auto& t : tracksAfter) {
                    if (t.isSelected) { currentId = t.id; currentTitle = t.title; currentLang = Utf16ToUtf8(t.lang); break; }
                }
                if (currentId > 0) {
                    session.SetLastActiveSubtitleTrackId(currentId);
                    session.SetLastActiveSubtitleTrackTitle(currentTitle);
                    settings.lastSubtitleTrackTitle = Utf16ToUtf8(currentTitle);
                    if (!currentLang.empty()) settings.subLang = currentLang;
                    settings.subtitlesEnabled = true;
                    player.SetSubtitleVisibility(true);
                    context->SaveSettings();
                    if (!session.GetCurrentFilePath().empty() && !session.IsPrivateMode()) { MediaLibrary& library = context->GetLibrary();
                    MediaItem* item = library.FindByPath(session.GetCurrentFilePath());
                    if (item) {
                        if (!item->preferences.has_value()) item->preferences.emplace();
                        item->preferences->subtitleTrackId = currentId;
                        item->preferences->subtitleTrackTitle = currentTitle;
                        if (!currentLang.empty()) item->preferences->subtitleTrackLang = currentLang;
                        item->preferences->subtitlesVisible = true;
                        session.SetActivePreferences(item->preferences);
                        context->SaveLibrary();
                    } }
                    context->ShowOSD(IconType::Subtitles, L"Subtitles: " + (currentTitle.empty() ? (L"Track " + std::to_wstring(currentId)) : currentTitle));
                } else {
                    session.SetLastActiveSubtitleTrackId(0);
                    session.SetLastActiveSubtitleTrackTitle(L"");
                    settings.subtitlesEnabled = false;
                    player.SetSubtitleVisibility(false);
                    context->SaveSettings();
                    if (!session.GetCurrentFilePath().empty() && !session.IsPrivateMode()) { MediaLibrary& library = context->GetLibrary();
                    MediaItem* item = library.FindByPath(session.GetCurrentFilePath());
                    if (item) {
                        if (!item->preferences.has_value()) item->preferences.emplace();
                        item->preferences->subtitleTrackId = 0;
                        item->preferences->subtitleTrackTitle = L"";
                        item->preferences->subtitlesVisible = false;
                        session.SetActivePreferences(item->preferences);
                        context->SaveLibrary();
                    } }
                    context->ShowOSD(IconType::Subtitles, L"Subtitles: Off");
                }
            }
            return true;
        }
        return false;
    }
    case AppCommand::SelectSubtitleTrack: {
        int targetId = args.intVal;
        std::wstring targetTitle = args.wstringVal;
        std::wstring targetLang = args.extraWstringVal;
        if (targetId == 0) {
            settings.subtitlesEnabled = false;
            session.SetLastActiveSubtitleTrackId(0);
            session.SetLastActiveSubtitleTrackTitle(L"");
            context->SaveSettings();
            player.SelectSubtitleTrack(0);
            player.SetSubtitleVisibility(false);
            UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) {
                prefs.subtitleTrackId = 0;
                prefs.subtitleTrackTitle = L"";
                prefs.subtitlesVisible = false;
            });
            context->ShowOSD(IconType::Subtitles, L"Subtitles: Off");
        } else {
            session.SetLastActiveSubtitleTrackId(targetId);
            session.SetLastActiveSubtitleTrackTitle(targetTitle);
            settings.lastSubtitleTrackTitle = Utf16ToUtf8(targetTitle);
            if (!targetLang.empty()) { settings.subLang = Utf16ToUtf8(targetLang); }
            settings.subtitlesEnabled = true;
            player.SetSubtitleVisibility(true);
            context->SaveSettings();
            player.SelectSubtitleTrack(targetId);
            UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) {
                prefs.subtitleTrackId = targetId;
                prefs.subtitleTrackTitle = targetTitle;
                if (!targetLang.empty()) prefs.subtitleTrackLang = Utf16ToUtf8(targetLang);
                prefs.subtitlesVisible = true;
            });
            context->ShowOSD(IconType::Subtitles, L"Subtitles: " + targetTitle);
        }
        return true;
    }
    case AppCommand::ToggleSubtitles: {
        if (!session.IsOpen()) return false;
        const auto tracks = player.GetSubtitleTracks();
        if (tracks.empty()) {
            context->ShowOSD(IconType::Subtitles, L"No Subtitles Available");
            return true;
        }
        const int currentId = player.GetCurrentSubtitleTrackId();
        if (player.GetSubtitleVisibility() && currentId > 0) {
            settings.subtitlesEnabled = false;
            player.SetSubtitleVisibility(false);
            context->SaveSettings();
            UpdateCurrentMediaPreferences(*context, [](MediaPlaybackPreferences& prefs) { prefs.subtitlesVisible = false; });
            context->ShowOSD(IconType::Subtitles, L"Subtitles: Off");
            return true;
        }
        const auto* selected = &tracks.front();
        const int restoreId = currentId > 0 ? currentId : session.GetLastActiveSubtitleTrackId();
        for (const auto& track : tracks) if (track.id == restoreId) { selected = &track; break; }
        return Execute(AppCommand::SelectSubtitleTrack, CommandArgs{ 0.0, selected->id, "", selected->title, selected->lang });
    }
    case AppCommand::CycleAudio: {
        if (session.IsOpen()) {
            player.CycleAudioTrack();
            int currentId = 0;
            std::wstring currentTitle = player.GetCurrentAudioTrackTitle();
            std::string currentLang;
            auto tracksAfter = player.GetAudioTracks();
            for (const auto& t : tracksAfter) {
                if (t.isSelected) { currentId = t.id; currentTitle = t.title; currentLang = Utf16ToUtf8(t.lang); break; }
            }
            if (currentId > 0) {
                session.SetLastActiveAudioTrackId(currentId);
                session.SetLastActiveAudioTrackTitle(currentTitle);
                settings.lastAudioTrackTitle = Utf16ToUtf8(currentTitle);
                if (!currentLang.empty()) settings.audioLang = currentLang;
                context->SaveSettings();
            }
            if (!session.GetCurrentFilePath().empty() && !session.IsPrivateMode()) { MediaLibrary& library = context->GetLibrary();
            MediaItem* item = library.FindByPath(session.GetCurrentFilePath());
            if (item) {
                if (!item->preferences.has_value()) item->preferences.emplace();
                item->preferences->audioTrackId = currentId;
                item->preferences->audioTrackTitle = currentTitle;
                if (!currentLang.empty()) item->preferences->audioTrackLang = currentLang;
                session.SetActivePreferences(item->preferences);
                context->SaveLibrary();
            } }
            context->ShowOSD(IconType::VolumeHigh, L"Audio: " + (currentTitle.empty() ? (L"Track " + std::to_wstring(currentId)) : currentTitle));
            return true;
        }
        return false;
    }
    case AppCommand::SelectAudioTrack: {
        int targetId = args.intVal;
        std::wstring targetTitle = args.wstringVal;
        std::wstring targetLang = args.extraWstringVal;
        session.SetLastActiveAudioTrackId(targetId);
        session.SetLastActiveAudioTrackTitle(targetTitle);
        settings.lastAudioTrackTitle = Utf16ToUtf8(targetTitle);
        if (!targetLang.empty()) { settings.audioLang = Utf16ToUtf8(targetLang); }
        context->SaveSettings();
        player.SelectAudioTrack(targetId);
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) {
            prefs.audioTrackId = targetId;
            prefs.audioTrackTitle = targetTitle;
            if (!targetLang.empty()) prefs.audioTrackLang = Utf16ToUtf8(targetLang);
        });
        context->ShowOSD(IconType::VolumeHigh, L"Audio: " + targetTitle);
        return true;
    }
    case AppCommand::RestartVideo: {
        if (session.IsOpen()) { player.Seek(0.0); player.Play(); context->ShowOSD(IconType::Play, L"Restarted"); return true; }
        return false;
    }
    case AppCommand::PlayPrevious: {
        if (session.IsQueuePlayback()) { context->PlayPrevInQueue(); return true; }
        return false;
    }
    case AppCommand::PlayNext: {
        if (session.IsQueuePlayback()) { context->PlayNextInQueue(); } else { context->PlayNextLibraryMedia(); }
        return true;
    }
    case AppCommand::NextChapter: {
        if (!session.IsOpen()) return false;
        auto chapters = player.GetChapters();
        if (chapters.empty()) { context->ShowOSD(IconType::Next, L"No Chapters"); return true; }
        int curChap = player.GetCurrentChapter();
        int targetIdx = (curChap >= 0 && curChap + 1 < (int)chapters.size()) ? curChap + 1 : (int)chapters.size() - 1;
        if (curChap < 0) {
            double curTime = player.GetCurrentTime();
            targetIdx = 0;
            for (size_t i = 0; i < chapters.size(); ++i) {
                if (chapters[i].time > curTime + 0.5) { targetIdx = (int)i; break; }
            }
        }
        player.NextChapter();
        std::wstring title = (targetIdx >= 0 && targetIdx < (int)chapters.size()) ? chapters[targetIdx].title : L"Next Chapter";
        if (title.length() > 60) title = title.substr(0, 57) + L"...";
        context->ShowOSD(IconType::Next, title);
        return true;
    }
    case AppCommand::PreviousChapter: {
        if (!session.IsOpen()) return false;
        auto chapters = player.GetChapters();
        if (chapters.empty()) { context->ShowOSD(IconType::Prev, L"No Chapters"); return true; }
        int curChap = player.GetCurrentChapter();
        double curTime = player.GetCurrentTime();
        int targetIdx = 0;
        if (curChap > 0) {
            if (curChap < (int)chapters.size() && (curTime - chapters[curChap].time) > 3.0) {
                targetIdx = curChap;
            } else { targetIdx = curChap - 1; }
        } else if (curChap == 0) { targetIdx = 0; } else {
            for (int i = (int)chapters.size() - 1; i >= 0; --i) {
                if (chapters[i].time < curTime - 0.5) { targetIdx = i; break; }
            }
        }
        player.PreviousChapter();
        std::wstring title = (targetIdx >= 0 && targetIdx < (int)chapters.size()) ? chapters[targetIdx].title : L"Previous Chapter";
        if (title.length() > 60) title = title.substr(0, 57) + L"...";
        context->ShowOSD(IconType::Prev, title);
        return true;
    }
    case AppCommand::CycleAspectRatio: {
        const char* aspects[] = { "auto", "16:9", "4:3", "21:9", "fill", "cover" };
        size_t index = 0;
        while (index < 6 && settings.lastAspectRatio != aspects[index]) ++index;
        index = (index + 1) % 6;
        settings.lastAspectRatio = aspects[index];
        player.SetAspectRatio(settings.lastAspectRatio);
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.aspectRatio = settings.lastAspectRatio; });
        context->ShowOSD(IconType::AspectFrame, L"Aspect: " + Utf8ToUtf16(settings.lastAspectRatio));
        return true;
    }
    case AppCommand::SetAspectRatio: {
        std::string aspect = args.stringVal;
        if (aspect.empty()) aspect = "auto";
        player.SetAspectRatio(aspect);
        settings.lastAspectRatio = aspect;
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.aspectRatio = aspect; });
        context->ShowOSD(IconType::AspectFrame, L"Aspect: " + Utf8ToUtf16(aspect));
        return true;
    }
    case AppCommand::RotateVideo: {
        int curRotate = player.GetVideoRotate();
        int newRotate = (curRotate + 90) % 360;
        player.SetVideoRotate(newRotate);
        settings.videoRotate = newRotate;
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.videoRotate = newRotate; });
        context->ShowOSD(IconType::AspectFrame, L"Rotation: " + std::to_wstring(newRotate) + L"°");
        return true;
    }
    case AppCommand::SetVideoRotate: {
        int rot = args.intVal;
        int normalized = ((rot % 360) + 360) % 360;
        if (normalized != 0 && normalized != 90 && normalized != 180 && normalized != 270) { normalized = 0; }
        player.SetVideoRotate(normalized);
        settings.videoRotate = normalized;
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.videoRotate = normalized; });
        context->ShowOSD(IconType::AspectFrame, L"Rotation: " + std::to_wstring(normalized) + L"°");
        return true;
    }
    case AppCommand::BrightnessPlus: return ApplyImageAdjustment(*context, args, kBrightness, ImageAdjustmentOperation::Increase);
    case AppCommand::BrightnessMinus: return ApplyImageAdjustment(*context, args, kBrightness, ImageAdjustmentOperation::Decrease);
    case AppCommand::BrightnessReset: return ApplyImageAdjustment(*context, args, kBrightness, ImageAdjustmentOperation::Reset);
    case AppCommand::SetBrightness: return ApplyImageAdjustment(*context, args, kBrightness, ImageAdjustmentOperation::Set);
    case AppCommand::ContrastPlus: return ApplyImageAdjustment(*context, args, kContrast, ImageAdjustmentOperation::Increase);
    case AppCommand::ContrastMinus: return ApplyImageAdjustment(*context, args, kContrast, ImageAdjustmentOperation::Decrease);
    case AppCommand::ContrastReset: return ApplyImageAdjustment(*context, args, kContrast, ImageAdjustmentOperation::Reset);
    case AppCommand::SetContrast: return ApplyImageAdjustment(*context, args, kContrast, ImageAdjustmentOperation::Set);
    case AppCommand::SaturationPlus: return ApplyImageAdjustment(*context, args, kSaturation, ImageAdjustmentOperation::Increase);
    case AppCommand::SaturationMinus: return ApplyImageAdjustment(*context, args, kSaturation, ImageAdjustmentOperation::Decrease);
    case AppCommand::SaturationReset: return ApplyImageAdjustment(*context, args, kSaturation, ImageAdjustmentOperation::Reset);
    case AppCommand::SetSaturation: return ApplyImageAdjustment(*context, args, kSaturation, ImageAdjustmentOperation::Set);
    case AppCommand::GammaPlus: return ApplyImageAdjustment(*context, args, kGamma, ImageAdjustmentOperation::Increase);
    case AppCommand::GammaMinus: return ApplyImageAdjustment(*context, args, kGamma, ImageAdjustmentOperation::Decrease);
    case AppCommand::GammaReset: return ApplyImageAdjustment(*context, args, kGamma, ImageAdjustmentOperation::Reset);
    case AppCommand::SetGamma: return ApplyImageAdjustment(*context, args, kGamma, ImageAdjustmentOperation::Set);
    case AppCommand::HuePlus: return ApplyImageAdjustment(*context, args, kHue, ImageAdjustmentOperation::Increase);
    case AppCommand::HueMinus: return ApplyImageAdjustment(*context, args, kHue, ImageAdjustmentOperation::Decrease);
    case AppCommand::HueReset: return ApplyImageAdjustment(*context, args, kHue, ImageAdjustmentOperation::Reset);
    case AppCommand::SetHue: return ApplyImageAdjustment(*context, args, kHue, ImageAdjustmentOperation::Set);
    case AppCommand::ResetVideoAdjustments: {
        player.SetVideoRotate(0);
        player.SetBrightness(0);
        player.SetContrast(0);
        player.SetSaturation(0);
        player.SetGamma(0);
        player.SetHue(0);
        settings.videoRotate = 0;
        settings.brightness = 0;
        settings.contrast = 0;
        settings.saturation = 0;
        settings.gamma = 0;
        settings.hue = 0;
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) {
            prefs.videoRotate = 0;
            prefs.brightness = 0;
            prefs.contrast = 0;
            prefs.saturation = 0;
            prefs.gamma = 0;
            prefs.hue = 0;
        });
        context->ShowOSD(IconType::Refresh, L"Video Adjustments Reset");
        return true;
    }
    case AppCommand::AudioDelayPlus: {
        settings.audioDelayMs = std::clamp(settings.audioDelayMs + 50, -5000, 5000);
        player.SetAudioDelay((double)settings.audioDelayMs / 1000.0);
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.audioDelayMs = settings.audioDelayMs; });
        context->ShowOSD(IconType::VolumeHigh, L"Audio Delay: " + std::to_wstring(settings.audioDelayMs) + L" ms");
        return true;
    }
    case AppCommand::AudioDelayMinus: {
        settings.audioDelayMs = std::clamp(settings.audioDelayMs - 50, -5000, 5000);
        player.SetAudioDelay((double)settings.audioDelayMs / 1000.0);
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.audioDelayMs = settings.audioDelayMs; });
        context->ShowOSD(IconType::VolumeHigh, L"Audio Delay: " + std::to_wstring(settings.audioDelayMs) + L" ms");
        return true;
    }
    case AppCommand::AudioDelayReset: {
        settings.audioDelayMs = 0;
        player.SetAudioDelay(0.0);
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.audioDelayMs = 0; });
        context->ShowOSD(IconType::VolumeHigh, L"Audio Delay: 0 ms");
        return true;
    }
    case AppCommand::SetAudioDelay: {
        settings.audioDelayMs = std::clamp(args.intVal, -5000, 5000);
        player.SetAudioDelay((double)settings.audioDelayMs / 1000.0);
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.audioDelayMs = settings.audioDelayMs; });
        context->ShowOSD(IconType::VolumeHigh, L"Audio Delay: " + std::to_wstring(settings.audioDelayMs) + L" ms");
        return true;
    }
    case AppCommand::AudioBalanceLeft: {
        settings.audioBalance = Settings::NormalizeAudioBalance(settings.audioBalance - 0.1);
        player.SetAudioBalance(settings.audioBalance);
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.audioBalance = settings.audioBalance; });
        std::wstring osdText;
        if (std::abs(settings.audioBalance) < 0.01) { osdText = L"Audio Balance: Center (0%)"; } else if (settings.audioBalance < 0.0) {
            wchar_t buf[64];
            swprintf_s(buf, L"Audio Balance: Left %.0f%%", -settings.audioBalance * 100.0);
            osdText = buf;
        } else { wchar_t buf[64]; swprintf_s(buf, L"Audio Balance: Right %.0f%%", settings.audioBalance * 100.0); osdText = buf; }
        context->ShowOSD(IconType::VolumeHigh, osdText);
        return true;
    }
    case AppCommand::AudioBalanceRight: {
        settings.audioBalance = Settings::NormalizeAudioBalance(settings.audioBalance + 0.1);
        player.SetAudioBalance(settings.audioBalance);
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.audioBalance = settings.audioBalance; });
        std::wstring osdText;
        if (std::abs(settings.audioBalance) < 0.01) { osdText = L"Audio Balance: Center (0%)"; } else if (settings.audioBalance < 0.0) {
            wchar_t buf[64];
            swprintf_s(buf, L"Audio Balance: Left %.0f%%", -settings.audioBalance * 100.0);
            osdText = buf;
        } else { wchar_t buf[64]; swprintf_s(buf, L"Audio Balance: Right %.0f%%", settings.audioBalance * 100.0); osdText = buf; }
        context->ShowOSD(IconType::VolumeHigh, osdText);
        return true;
    }
    case AppCommand::AudioBalanceReset: {
        settings.audioBalance = 0.0;
        player.SetAudioBalance(0.0);
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.audioBalance = 0.0; });
        context->ShowOSD(IconType::VolumeHigh, L"Audio Balance: Center (Reset)");
        return true;
    }
    case AppCommand::SetAudioBalance: {
        settings.audioBalance = Settings::NormalizeAudioBalance(args.doubleVal);
        player.SetAudioBalance(settings.audioBalance);
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.audioBalance = settings.audioBalance; });
        std::wstring osdText;
        if (std::abs(settings.audioBalance) < 0.01) { osdText = L"Audio Balance: Center"; } else if (settings.audioBalance < 0.0) {
            wchar_t buf[64];
            swprintf_s(buf, L"Audio Balance: Left %.0f%%", -settings.audioBalance * 100.0);
            osdText = buf;
        } else { wchar_t buf[64]; swprintf_s(buf, L"Audio Balance: Right %.0f%%", settings.audioBalance * 100.0); osdText = buf; }
        context->ShowOSD(IconType::VolumeHigh, osdText);
        return true;
    }
    case AppCommand::CycleAudioChannelMap: {
        std::string cur = Settings::NormalizeAudioChannelMap(settings.audioChannelMap);
        std::string next = "auto";
        if (cur == "auto") next = "stereo";
        else if (cur == "stereo") next = "mono-left";
        else if (cur == "mono-left") next = "mono-right";
        else if (cur == "mono-right") next = "swap";
        else if (cur == "swap") next = "auto";
        settings.audioChannelMap = next;
        player.SetAudioChannelMap(settings.audioChannelMap);
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.audioChannelMap = settings.audioChannelMap; });
        std::wstring mapDesc = L"Auto (Default)";
        if (next == "stereo") mapDesc = L"Stereo";
        else if (next == "mono-left") mapDesc = L"Mono (Left Channel)";
        else if (next == "mono-right") mapDesc = L"Mono (Right Channel)";
        else if (next == "swap") mapDesc = L"Swap (Left ↔ Right)";
        context->ShowOSD(IconType::VolumeHigh, L"Channel Map: " + mapDesc);
        return true;
    }
    case AppCommand::SetAudioChannelMap: {
        settings.audioChannelMap = Settings::NormalizeAudioChannelMap(args.stringVal);
        player.SetAudioChannelMap(settings.audioChannelMap);
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.audioChannelMap = settings.audioChannelMap; });
        std::wstring mapDesc = L"Auto (Default)";
        if (settings.audioChannelMap == "stereo") mapDesc = L"Stereo";
        else if (settings.audioChannelMap == "mono-left") mapDesc = L"Mono (Left Channel)";
        else if (settings.audioChannelMap == "mono-right") mapDesc = L"Mono (Right Channel)";
        else if (settings.audioChannelMap == "swap") mapDesc = L"Swap (Left ↔ Right)";
        context->ShowOSD(IconType::VolumeHigh, L"Channel Map: " + mapDesc);
        return true;
    }
    case AppCommand::AudioChannelMapReset: {
        settings.audioChannelMap = "auto";
        player.SetAudioChannelMap("auto");
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.audioChannelMap = "auto"; });
        context->ShowOSD(IconType::VolumeHigh, L"Channel Map: Auto (Reset)");
        return true;
    }
    case AppCommand::ResetAudioBalanceAndChannelMap: {
        settings.audioBalance = 0.0;
        settings.audioChannelMap = "auto";
        player.SetAudioPreferences(settings.audioDevice, settings.audioLayout, settings.audioChannelMix, settings.audioNormalizeDownmix, settings.nightMode, 0.0, "auto");
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) {
            prefs.audioBalance = 0.0;
            prefs.audioChannelMap = "auto";
        });
        context->ShowOSD(IconType::Refresh, L"Audio Balance & Channel Map Reset");
        return true;
    }
    case AppCommand::SubDelayPlus: {
        settings.subDelayMs = std::clamp(settings.subDelayMs + 100, -10000, 10000);
        player.SetSubDelay((double)settings.subDelayMs / 1000.0);
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.subDelayMs = settings.subDelayMs; });
        context->ShowOSD(IconType::Subtitles, L"Subtitle Delay: " + std::to_wstring(settings.subDelayMs) + L" ms");
        return true;
    }
    case AppCommand::SubDelayMinus: {
        settings.subDelayMs = std::clamp(settings.subDelayMs - 100, -10000, 10000);
        player.SetSubDelay((double)settings.subDelayMs / 1000.0);
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.subDelayMs = settings.subDelayMs; });
        context->ShowOSD(IconType::Subtitles, L"Subtitle Delay: " + std::to_wstring(settings.subDelayMs) + L" ms");
        return true;
    }
    case AppCommand::SubDelayReset: {
        settings.subDelayMs = 0;
        player.SetSubDelay(0.0);
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.subDelayMs = 0; });
        context->ShowOSD(IconType::Subtitles, L"Subtitle Delay: 0 ms");
        return true;
    }
    case AppCommand::SetSubDelay: {
        settings.subDelayMs = std::clamp(args.intVal, -10000, 10000);
        player.SetSubDelay((double)settings.subDelayMs / 1000.0);
        context->SaveSettings();
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.subDelayMs = settings.subDelayMs; });
        context->ShowOSD(IconType::Subtitles, L"Subtitle Delay: " + std::to_wstring(settings.subDelayMs) + L" ms");
        return true;
    }
    case AppCommand::ClosePlayer: {
        if (context->ArePopupsOpen()) { context->DismissPopups(); context->RequestRender(); } else if (session.IsFullscreen()) {
            context->ToggleFullscreen();
        } else { context->ClosePlayer(); }
        return true;
    }
    case AppCommand::FrameStepForward: {
        if (session.IsOpen()) {
            player.Pause();
            player.StepFrame(settings.ctrlSeekFrames);
            context->ShowOSD(IconType::Next, L"Step Frame Forward");
            return true;
        }
        return false;
    }
    case AppCommand::FrameStepBackward: {
        if (session.IsOpen()) {
            player.Pause();
            player.StepFrame(-settings.ctrlSeekFrames);
            context->ShowOSD(IconType::Prev, L"Step Frame Backward");
            return true;
        }
        return false;
    }
    case AppCommand::SpeedUp: {
        double cur = player.GetPlaybackRate();
        double next = std::min(4.0, cur + 0.25);
        player.SetPlaybackRate(next);
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.playbackSpeed = next; });
        wchar_t buf[32];
        swprintf_s(buf, L"Speed: %.2fx", next);
        context->ShowOSD(IconType::SpeedGauge, buf);
        return true;
    }
    case AppCommand::SpeedDown: {
        double cur = player.GetPlaybackRate();
        double next = std::max(0.25, cur - 0.25);
        player.SetPlaybackRate(next);
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.playbackSpeed = next; });
        wchar_t buf[32];
        swprintf_s(buf, L"Speed: %.2fx", next);
        context->ShowOSD(IconType::SpeedGauge, buf);
        return true;
    }
    case AppCommand::SpeedReset: {
        player.SetPlaybackRate(1.0);
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.playbackSpeed = 1.0; });
        context->ShowOSD(IconType::SpeedGauge, L"Speed: 1.00x");
        return true;
    }
    case AppCommand::SetSpeed: {
        if (!std::isfinite(args.doubleVal)) return false;
        double speed = std::clamp(args.doubleVal, 0.25, 4.0);
        player.SetPlaybackRate(speed);
        UpdateCurrentMediaPreferences(*context, [&](MediaPlaybackPreferences& prefs) { prefs.playbackSpeed = speed; });
        wchar_t buf[32];
        swprintf_s(buf, L"Speed: %.2fx", speed);
        context->ShowOSD(IconType::SpeedGauge, buf);
        return true;
    }
    case AppCommand::ToggleLoop: {
        settings.loop = !settings.loop;
        player.SetLoop(settings.loop);
        context->SaveSettings();
        context->ShowOSD(IconType::LoopRepeat, settings.loop ? L"Loop: On" : L"Loop: Off");
        return true;
    }
    case AppCommand::ToggleAutoplay: {
        settings.autoplay = !settings.autoplay;
        context->SaveSettings();
        context->ShowOSD(IconType::AutoplayNext, settings.autoplay ? L"Autoplay: On" : L"Autoplay: Off");
        return true;
    }
    case AppCommand::OpenFile: { context->OpenFileDialog(); return true; }
    case AppCommand::OpenSubtitleFile: { context->OpenSubtitleFileDialog(); return true; }
    case AppCommand::SearchOnlineSubtitles: { context->OpenOnlineSubtitlesDialog(); return true; }
    case AppCommand::ShowInExplorer: {
        std::wstring targetPath = args.wstringVal.empty() ? session.GetCurrentFilePath() : args.wstringVal;
        if (!targetPath.empty()) { context->OpenInExplorer(targetPath); return true; }
        return false;
    }
    case AppCommand::TogglePlaybackDiagnostics: {
        if (session.IsOpen()) {
            context->TogglePlaybackDiagnostics();
            bool isVisible = context->IsPlaybackDiagnosticsVisible();
            context->ShowOSD(IconType::SettingsGear, isVisible ? L"Diagnostics: On" : L"Diagnostics: Off", 1.0f);
            context->RequestRender();
        }
        return true;
    }
    case AppCommand::TakeScreenshot: {
        if (!session.IsOpen()) { context->ShowOSD(IconType::VolumeMuted, L"Screenshot unavailable: no media loaded", 2.0f); return false; }
        // Semantics: Explicit user-initiated screenshot captures remain permitted in private mode,
        // saving the requested image file directly to disk while strictly suppressing all
        // automatic playback-derived traces (history, progress, resume, thumbnails, timestamps).
        return context->TakeScreenshot();
    }
    case AppCommand::TogglePrivateMode: { context->TogglePrivateMode(); return true; }
    default:
        return false;
    }
}

} // namespace Aperture
