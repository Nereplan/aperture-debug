#pragma once

#include "Common.h"
#include "Settings.h"
#include "PlaybackSession.h"
#include "Renderer.h"
#include <string>
#include <vector>
#include <functional>

namespace Aperture {

class MpvPlayer;
class MediaLibrary;

enum class AppCommand {
    None = 0, TogglePlay, Play, Pause, SeekForward, SeekBackward, SeekForwardLarge, SeekBackwardLarge, SeekForwardMedium,
    SeekBackwardMedium, SeekTo, VolumeUp, VolumeDown,
    SetVolume, ToggleMute, SetMuted, ToggleFullscreen, TogglePiP, CycleSubtitles, SelectSubtitleTrack, ToggleSubtitles, CycleAudio,
    SelectAudioTrack, RestartVideo, PlayNext, PlayPrevious, NextChapter, PreviousChapter, CycleAspectRatio, SetAspectRatio, RotateVideo,
    SetVideoRotate, BrightnessPlus, BrightnessMinus, BrightnessReset, SetBrightness, ContrastPlus, ContrastMinus, ContrastReset,
    SetContrast, SaturationPlus, SaturationMinus, SaturationReset, SetSaturation, GammaPlus, GammaMinus, GammaReset, SetGamma, HuePlus,
    HueMinus, HueReset, SetHue, ResetVideoAdjustments, AudioDelayPlus, AudioDelayMinus, AudioDelayReset, SetAudioDelay, AudioBalanceLeft,
    AudioBalanceRight, AudioBalanceReset, SetAudioBalance, CycleAudioChannelMap, SetAudioChannelMap, AudioChannelMapReset,
    ResetAudioBalanceAndChannelMap, SubDelayPlus, SubDelayMinus, SubDelayReset, SetSubDelay, ClosePlayer, FrameStepForward,
    FrameStepBackward, SpeedUp, SpeedDown, SpeedReset, SetSpeed, ToggleLoop, ToggleAutoplay, OpenFile, OpenSubtitleFile,
    SearchOnlineSubtitles, ShowInExplorer, TogglePlaybackDiagnostics, TakeScreenshot, TogglePrivateMode, ToggleShuffle,
    ToggleAlwaysOnTop, JumpToTime, ToggleLogViewer, SubSyncMarkAudio, SubSyncMarkSubtitle, SubSyncReset
};

// Mouse-button/wheel pseudo-key codes used by shortcut bindings. They live in a
// dedicated range above real virtual-key codes so they can never collide with
// keyboard chords; only Shift/Ctrl/Alt held at click time combine with them.
inline constexpr UINT kMouseMiddle = 0x100, kMouseX1 = 0x101, kMouseX2 = 0x102, kMouseWheelUp = 0x103, kMouseWheelDown = 0x104;
inline const wchar_t* MouseBindingDisplayName(UINT vkCode) {
    switch (vkCode) {
        case kMouseMiddle: return L"Middle Click";
        case kMouseX1: return L"X Button 1";
        case kMouseX2: return L"X Button 2";
        case kMouseWheelUp: return L"Wheel Up";
        case kMouseWheelDown: return L"Wheel Down";
        default: return nullptr;
    }
}

struct CommandArgs {
    double doubleVal = 0.0;
    int intVal = 0;
    std::string stringVal;
    std::wstring wstringVal, extraWstringVal;
};

class ICommandContext {
public:
    virtual ~ICommandContext() = default;
    virtual Settings& GetSettings() = 0;
    virtual MpvPlayer& GetPlayer() = 0;
    virtual PlaybackSession& GetPlaybackSession() = 0;
    virtual MediaLibrary& GetLibrary() = 0;
    virtual void ShowOSD(IconType icon, const std::wstring& text, float durationSec = 1.5f) = 0;
    virtual void TriggerDoubleTapFeedback(bool isForward, int deltaSeconds = 5) = 0;
    virtual void TriggerPlayPauseBadge(bool isPlay) = 0;
    virtual bool SaveSettings() = 0;
    virtual bool SaveLibrary() = 0;
    virtual void OpenAndPlayMedia(const std::wstring& filePath, double startPosSec = 0.0, bool preserveQueue = false) = 0;
    virtual void PlayFirstInQueue() = 0;
    virtual void PlayNextInQueue() = 0;
    virtual void PlayPrevInQueue() = 0;
    virtual void PlayNextLibraryMedia() = 0;
    virtual void ToggleFullscreen() = 0;
    virtual void TogglePiP() = 0;
    virtual void ClosePlayer() = 0;
    virtual void OpenFileDialog() = 0;
    virtual void OpenSubtitleFileDialog() {}
    virtual void OpenOnlineSubtitlesDialog() {}
    virtual void OpenInExplorer(const std::wstring& filePath) = 0;
    virtual bool TakeScreenshot() = 0;
    virtual void DismissPopups() = 0;
    virtual bool ArePopupsOpen() const = 0;
    virtual void RequestRender() = 0;
    virtual void TogglePlaybackDiagnostics() = 0;
    virtual bool IsPlaybackDiagnosticsVisible() const = 0;
    virtual void TogglePrivateMode() {}
    virtual void ToggleAlwaysOnTop() {}
    virtual void ShowJumpToTimeModal() {}
    virtual void ToggleLogViewer() {}
    // Guided subtitle-sync mark points. -1.0 means no audio point is armed.
    virtual double GetSubSyncAudioPoint() { return -1.0; }
    virtual void SetSubSyncAudioPoint(double sec) {}
};

class CommandDispatcher {
public:
    CommandDispatcher() = default;
    explicit CommandDispatcher(ICommandContext* context);
    void SetContext(ICommandContext* context) { this->context = context; }
    // Canonical Execution
    bool Execute(AppCommand cmd, const CommandArgs& args = {});
    bool Execute(const std::string& actionId, const CommandArgs& args = {});
    // Keyboard Translation
    AppCommand TranslateKeyChord(UINT vkCode, bool isCtrl, bool isShift, bool isAlt, bool isPlayerOpen, const Settings& settings) const;
    // Mouse Translation (player context): exact-match binding lookup for mouse
    // pseudo-keys. For wheel bindings, a chord with no modifiers matches a bare wheel.
    AppCommand FindMouseBinding(UINT vkCode, bool isCtrl, bool isShift, bool isAlt, const Settings& settings) const;
    // String / Enum Conversions
    static AppCommand CommandIdFromString(const std::string& actionId);
    static std::string StringFromCommandId(AppCommand cmd);

private:
    ICommandContext* context = nullptr;
};

} // namespace Aperture
