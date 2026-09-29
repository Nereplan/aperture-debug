#pragma once

#include "../support/TestFramework.h"
#include "../../src/CommandDispatcher.h"
#include "../../src/MpvPlayer.h"
#include "../../src/MediaLibrary.h"

namespace Aperture::Test {

class MockCommandContext : public ICommandContext {
public:
    Settings settings;
    MpvPlayer player;
    PlaybackSession session;
    MediaLibrary library;

    bool osdShown = false;
    IconType lastOsdIcon = IconType::VolumeHigh;
    std::wstring lastOsdText;

    bool doubleTapTriggered = false;
    bool doubleTapForward = false;
    int doubleTapDelta = 0;

    bool playPauseBadgeTriggered = false;
    bool playPauseBadgeIsPlay = false;

    bool settingsSaved = false;
    bool librarySaved = false;

    bool openAndPlayCalled = false;
    std::wstring lastOpenPath;
    double lastOpenStartPos = 0.0;
    bool lastOpenPreserveQueue = false;

    bool playFirstInQueueCalled = false;
    bool playNextInQueueCalled = false;
    bool playPrevInQueueCalled = false;
    bool playNextLibraryMediaCalled = false;

    bool toggleFullscreenCalled = false;
    bool togglePiPCalled = false;
    bool closePlayerCalled = false;

    bool openFileDialogCalled = false;
    bool openSubtitleFileDialogCalled = false;
    bool openInExplorerCalled = false;
    std::wstring lastExplorerPath;
    bool takeScreenshotCalled = false;

    bool popupsDismissed = false;
    bool popupsOpen = false;
    bool renderRequested = false;
    bool playbackDiagnosticsVisible = false;

    Settings& GetSettings() override { return settings; }
    MpvPlayer& GetPlayer() override { return player; }
    PlaybackSession& GetPlaybackSession() override { return session; }
    MediaLibrary& GetLibrary() override { return library; }

    void ShowOSD(IconType icon, const std::wstring& text, float) override {
        osdShown = true;
        lastOsdIcon = icon;
        lastOsdText = text;
    }

    void TriggerDoubleTapFeedback(bool isForward, int deltaSeconds) override {
        doubleTapTriggered = true;
        doubleTapForward = isForward;
        doubleTapDelta = deltaSeconds;
    }

    void TriggerPlayPauseBadge(bool isPlay) override {
        playPauseBadgeTriggered = true;
        playPauseBadgeIsPlay = isPlay;
    }

    bool SaveSettings() override {
        settingsSaved = true;
        return true;
    }

    bool SaveLibrary() override {
        librarySaved = true;
        return true;
    }

    void OpenAndPlayMedia(const std::wstring& filePath, double startPosSec, bool preserveQueue) override {
        openAndPlayCalled = true;
        lastOpenPath = filePath;
        lastOpenStartPos = startPosSec;
        lastOpenPreserveQueue = preserveQueue;
    }

    void PlayFirstInQueue() override { playFirstInQueueCalled = true; }
    void PlayNextInQueue() override { playNextInQueueCalled = true; }
    void PlayPrevInQueue() override { playPrevInQueueCalled = true; }
    void PlayNextLibraryMedia() override { playNextLibraryMediaCalled = true; }

    void ToggleFullscreen() override { toggleFullscreenCalled = true; }
    void TogglePiP() override { togglePiPCalled = true; }
    void ClosePlayer() override { closePlayerCalled = true; }

    void OpenFileDialog() override { openFileDialogCalled = true; }
    void OpenSubtitleFileDialog() override { openSubtitleFileDialogCalled = true; }
    void OpenInExplorer(const std::wstring& filePath) override {
        openInExplorerCalled = true;
        lastExplorerPath = filePath;
    }
    bool TakeScreenshot() override {
        takeScreenshotCalled = true;
        return true;
    }

    void DismissPopups() override { popupsDismissed = true; popupsOpen = false; }
    bool ArePopupsOpen() const override { return popupsOpen; }
    void RequestRender() override { renderRequested = true; }
    void TogglePlaybackDiagnostics() override { playbackDiagnosticsVisible = !playbackDiagnosticsVisible; }
    bool IsPlaybackDiagnosticsVisible() const override { return playbackDiagnosticsVisible; }
};

TEST_CASE(CommandDispatcher, StringAndEnumConversions) {
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("togglePlay"), AppCommand::TogglePlay);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("seekForward"), AppCommand::SeekForward);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("seekBackward"), AppCommand::SeekBackward);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("volumeUp"), AppCommand::VolumeUp);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("volumeDown"), AppCommand::VolumeDown);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("toggleMute"), AppCommand::ToggleMute);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("toggleFullscreen"), AppCommand::ToggleFullscreen);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("togglePiP"), AppCommand::TogglePiP);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("cycleSubtitles"), AppCommand::CycleSubtitles);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("cycleAudio"), AppCommand::CycleAudio);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("restartVideo"), AppCommand::RestartVideo);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("playNext"), AppCommand::PlayNext);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("playPrevious"), AppCommand::PlayPrevious);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("closePlayer"), AppCommand::ClosePlayer);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("togglePlaybackDiagnostics"), AppCommand::TogglePlaybackDiagnostics);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("invalidCommand"), AppCommand::None);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("takeScreenshot"), AppCommand::TakeScreenshot);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("screenshot"), AppCommand::TakeScreenshot);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("captureScreenshot"), AppCommand::TakeScreenshot);

    TEST_ASSERT_EQ(CommandDispatcher::StringFromCommandId(AppCommand::TogglePlay), std::string("togglePlay"));
    TEST_ASSERT_EQ(CommandDispatcher::StringFromCommandId(AppCommand::SpeedUp), std::string("speedUp"));
    TEST_ASSERT_EQ(CommandDispatcher::StringFromCommandId(AppCommand::AudioDelayPlus), std::string("audioDelayPlus"));
    TEST_ASSERT_EQ(CommandDispatcher::StringFromCommandId(AppCommand::None), std::string(""));
    TEST_ASSERT_EQ(CommandDispatcher::StringFromCommandId(AppCommand::TogglePlaybackDiagnostics), std::string("togglePlaybackDiagnostics"));
    TEST_ASSERT_EQ(CommandDispatcher::StringFromCommandId(AppCommand::TakeScreenshot), std::string("takeScreenshot"));
}

TEST_CASE(CommandDispatcher, KeyChordTranslation) {
    CommandDispatcher dispatcher;
    Settings settings;

    // Custom bindings take precedence
    ShortcutBinding customPlay;
    customPlay.actionId = "togglePlay";
    customPlay.vkCode = 'Z';
    customPlay.ctrl = true;
    settings.shortcuts["togglePlay"] = customPlay;

    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('Z', true, false, false, true, settings), AppCommand::TogglePlay);

    // Fallbacks when player is open
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_SPACE, false, false, false, true, settings), AppCommand::TogglePlay);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('K', false, false, false, true, settings), AppCommand::TogglePlay);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_LEFT, false, false, false, true, settings), AppCommand::SeekBackward);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_RIGHT, false, false, false, true, settings), AppCommand::SeekForward);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('J', false, false, false, true, settings), AppCommand::SeekBackwardLarge);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('L', false, false, false, true, settings), AppCommand::SeekForwardLarge);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_UP, false, false, false, true, settings), AppCommand::VolumeUp);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_DOWN, false, false, false, true, settings), AppCommand::VolumeDown);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('M', false, false, false, true, settings), AppCommand::ToggleMute);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('F', false, false, false, true, settings), AppCommand::ToggleFullscreen);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('P', false, false, false, true, settings), AppCommand::TogglePiP);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('S', false, false, false, true, settings), AppCommand::CycleSubtitles);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('A', false, false, false, true, settings), AppCommand::CycleAudio);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('R', false, false, false, true, settings), AppCommand::RestartVideo);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('N', false, false, false, true, settings), AppCommand::PlayNext);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_ESCAPE, false, false, false, true, settings), AppCommand::ClosePlayer);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('I', false, false, false, true, settings), AppCommand::TogglePlaybackDiagnostics);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('D', false, false, false, true, settings), AppCommand::TogglePlaybackDiagnostics);

    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('S', true, false, false, true, settings), AppCommand::TakeScreenshot);
    // Default delay shortcuts in settings
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_LEFT, true, false, false, true, settings), AppCommand::AudioDelayMinus);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_RIGHT, true, false, false, true, settings), AppCommand::AudioDelayPlus);

    // Ctrl+Left / Ctrl+Right frame step fallback when no custom/delay shortcut claimed the chord
    Settings fallbackSettings = settings;
    fallbackSettings.shortcuts.erase("audioDelayMinus");
    fallbackSettings.shortcuts.erase("audioDelayPlus");
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_LEFT, true, false, false, true, fallbackSettings), AppCommand::FrameStepBackward);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_RIGHT, true, false, false, true, fallbackSettings), AppCommand::FrameStepForward);
    // Fallbacks disabled when player is closed
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_SPACE, false, false, false, false, settings), AppCommand::None);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_LEFT, false, false, false, false, settings), AppCommand::None);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('F', false, false, false, false, settings), AppCommand::None);
}

TEST_CASE(CommandDispatcher, VolumeAndMuteExecution) {
    MockCommandContext context;
    CommandDispatcher dispatcher(&context);

    context.settings.volume = 0.5;
    context.player.SetVolume(0.5);
    context.session.OpenDirect(L"C:\\test.mp4");
    context.session.OnPlayerOpened(true);

    // Volume Up increases by 0.05 and shows OSD
    dispatcher.Execute(AppCommand::VolumeUp);
    TEST_ASSERT_EQ(std::round(context.settings.volume * 100.0), 55.0);
    TEST_ASSERT(context.osdShown);
    TEST_ASSERT_EQ(context.lastOsdText, std::wstring(L"55%"));

    // Volume Down decreases by 0.05
    dispatcher.Execute(AppCommand::VolumeDown);
    TEST_ASSERT_EQ(std::round(context.settings.volume * 100.0), 50.0);

    // Set Volume clamping
    CommandArgs highVol;
    highVol.doubleVal = 1.5;
    dispatcher.Execute(AppCommand::SetVolume, highVol);
    TEST_ASSERT_EQ(context.settings.volume, 1.0);

    CommandArgs lowVol;
    lowVol.doubleVal = -0.5;
    dispatcher.Execute(AppCommand::SetVolume, lowVol);
    TEST_ASSERT_EQ(context.settings.volume, 0.0);

    // Toggle Mute from unmuted state
    context.settings.volume = 0.8;
    context.player.SetVolume(0.8);
    context.player.SetMuted(false);
    context.settings.muted = false;
    context.session.SetPriorUnmutedVolume(0.8);
    dispatcher.Execute(AppCommand::ToggleMute);
    TEST_ASSERT(context.settings.muted);
    TEST_ASSERT_EQ(context.lastOsdText, std::wstring(L"Muted"));
    // Unmute restores volume
    dispatcher.Execute(AppCommand::ToggleMute);
    TEST_ASSERT(!context.settings.muted);
    TEST_ASSERT(context.lastOsdText.find(L"Unmuted") != std::wstring::npos);
}


TEST_CASE(CommandDispatcher, PlaybackDiagnosticsExecution) {
    MockCommandContext context;
    CommandDispatcher dispatcher(&context);

    context.session.OpenDirect(L"C:\\test.mp4");
    context.session.OnPlayerOpened(true);

    TEST_ASSERT(!context.playbackDiagnosticsVisible);
    dispatcher.Execute(AppCommand::TogglePlaybackDiagnostics);
    TEST_ASSERT(context.playbackDiagnosticsVisible);
    TEST_ASSERT(context.renderRequested);
    TEST_ASSERT(context.osdShown);
    TEST_ASSERT(context.lastOsdText.find(L"Diagnostics") != std::wstring::npos);

    dispatcher.Execute(AppCommand::TogglePlaybackDiagnostics);
    TEST_ASSERT(!context.playbackDiagnosticsVisible);
}

TEST_CASE(CommandDispatcher, NavigationAndDismissalPrecedence) {
    MockCommandContext context;
    CommandDispatcher dispatcher(&context);

    // 1. Popups open -> ClosePlayer dismisses popups first without closing player
    context.session.OpenDirect(L"C:\\test.mp4");
    context.session.OnPlayerOpened(true);
    context.popupsOpen = true;

    dispatcher.Execute(AppCommand::ClosePlayer);
    TEST_ASSERT(context.popupsDismissed);
    TEST_ASSERT(!context.closePlayerCalled);
    TEST_ASSERT(context.renderRequested);

    // 2. PlayNext with active queue routes to PlayNextInQueue
    context.session.OpenQueue({ L"C:\\1.mp4", L"C:\\2.mp4" }, 0);
    dispatcher.Execute(AppCommand::PlayNext);
    TEST_ASSERT(context.playNextInQueueCalled);
    TEST_ASSERT(!context.playNextLibraryMediaCalled);

    // 3. PlayNext without queue routes to PlayNextLibraryMedia
    context.session.OpenDirect(L"C:\\movie.mp4");
    context.playNextInQueueCalled = false;
    context.playNextLibraryMediaCalled = false;
    dispatcher.Execute(AppCommand::PlayNext);
    TEST_ASSERT(!context.playNextInQueueCalled);
    TEST_ASSERT(context.playNextLibraryMediaCalled);
}

TEST_CASE(CommandDispatcher, PerMediaPreferenceCommandUpdates) {
    MockCommandContext context;
    CommandDispatcher dispatcher(&context);

    std::wstring filePath = L"C:\\media\\show_ep1.mkv";
    context.session.OpenDirect(filePath, 0.0);
    context.session.OnPlayerOpened(true);

    MediaItem* item = context.library.AddOrUpdateMedia(filePath);
    TEST_ASSERT(item != nullptr);
    TEST_ASSERT(!item->preferences.has_value());

    // 1. Explicit Speed Change
    dispatcher.Execute(AppCommand::SetSpeed, CommandArgs{ 1.5 });
    TEST_ASSERT(item->preferences.has_value());
    TEST_ASSERT(item->preferences->playbackSpeed.has_value());
    TEST_ASSERT_EQ(*item->preferences->playbackSpeed, 1.5);
    TEST_ASSERT(context.session.GetActivePreferences().has_value());
    TEST_ASSERT_EQ(*context.session.GetActivePreferences()->playbackSpeed, 1.5);
    TEST_ASSERT(context.librarySaved);

    // 2. Explicit Aspect Ratio Change
    context.librarySaved = false;
    dispatcher.Execute(AppCommand::SetAspectRatio, CommandArgs{ 0.0, 0, "16:9" });
    TEST_ASSERT(item->preferences->aspectRatio.has_value());
    TEST_ASSERT_EQ(*item->preferences->aspectRatio, std::string("16:9"));
    TEST_ASSERT_EQ(*context.session.GetActivePreferences()->aspectRatio, std::string("16:9"));
    TEST_ASSERT(context.librarySaved);

    // 3. Explicit Audio Delay Change
    context.librarySaved = false;
    dispatcher.Execute(AppCommand::SetAudioDelay, CommandArgs{ 0.0, 250 });
    TEST_ASSERT(item->preferences->audioDelayMs.has_value());
    TEST_ASSERT_EQ(*item->preferences->audioDelayMs, 250);
    TEST_ASSERT_EQ(*context.session.GetActivePreferences()->audioDelayMs, 250);
    TEST_ASSERT(context.librarySaved);

    // 4. Explicit Subtitle Delay Change
    context.librarySaved = false;
    dispatcher.Execute(AppCommand::SetSubDelay, CommandArgs{ 0.0, -300 });
    TEST_ASSERT(item->preferences->subDelayMs.has_value());
    TEST_ASSERT_EQ(*item->preferences->subDelayMs, -300);
    TEST_ASSERT_EQ(*context.session.GetActivePreferences()->subDelayMs, -300);
    TEST_ASSERT(context.librarySaved);

    // 5. Explicit Subtitle Track Selection
    context.librarySaved = false;
    dispatcher.Execute(AppCommand::SelectSubtitleTrack, CommandArgs{ 0.0, 2, "", L"Japanese SDH", L"jpn" });
    TEST_ASSERT(item->preferences->subtitleTrackId.has_value());
    TEST_ASSERT_EQ(*item->preferences->subtitleTrackId, 2);
    TEST_ASSERT_EQ(*item->preferences->subtitleTrackTitle, std::wstring(L"Japanese SDH"));
    TEST_ASSERT_EQ(*item->preferences->subtitleTrackLang, std::string("jpn"));
    TEST_ASSERT_EQ(*item->preferences->subtitlesVisible, true);
    TEST_ASSERT(context.librarySaved);

    // 6. Explicit Subtitle Track Disable
    context.librarySaved = false;
    dispatcher.Execute(AppCommand::SelectSubtitleTrack, CommandArgs{ 0.0, 0, "", L"", L"" });
    TEST_ASSERT_EQ(*item->preferences->subtitleTrackId, 0);
    TEST_ASSERT_EQ(*item->preferences->subtitlesVisible, false);
    TEST_ASSERT(context.librarySaved);

    // 7. Explicit Audio Track Selection
    context.librarySaved = false;
    dispatcher.Execute(AppCommand::SelectAudioTrack, CommandArgs{ 0.0, 1, "", L"Original Audio", L"eng" });
    TEST_ASSERT(item->preferences->audioTrackId.has_value());
    TEST_ASSERT_EQ(*item->preferences->audioTrackId, 1);
    TEST_ASSERT_EQ(*item->preferences->audioTrackTitle, std::wstring(L"Original Audio"));
    TEST_ASSERT_EQ(*item->preferences->audioTrackLang, std::string("eng"));
    TEST_ASSERT(context.librarySaved);
}

TEST_CASE(CommandDispatcher, ChapterCommandConversionsAndKeyChords) {
    MockCommandContext context;
    CommandDispatcher dispatcher(&context);

    // 1. String <-> Enum conversions
    TEST_ASSERT_EQ(CommandDispatcher::StringFromCommandId(AppCommand::NextChapter), std::string("nextChapter"));
    TEST_ASSERT_EQ(CommandDispatcher::StringFromCommandId(AppCommand::PreviousChapter), std::string("previousChapter"));
    TEST_ASSERT(CommandDispatcher::CommandIdFromString("nextChapter") == AppCommand::NextChapter);
    TEST_ASSERT(CommandDispatcher::CommandIdFromString("previousChapter") == AppCommand::PreviousChapter);
    TEST_ASSERT(CommandDispatcher::CommandIdFromString("prevChapter") == AppCommand::PreviousChapter);

    // 2. Display names
    TEST_ASSERT_EQ(CommandDispatcher::GetCommandDefaultDisplayName(AppCommand::NextChapter), std::wstring(L"Next Chapter"));
    TEST_ASSERT_EQ(CommandDispatcher::GetCommandDefaultDisplayName(AppCommand::PreviousChapter), std::wstring(L"Previous Chapter"));

    // 3. Key Chord Translation
    Settings s;
    s.ResetShortcuts();

    // Default PageDown / Shift+P
    TEST_ASSERT(dispatcher.TranslateKeyChord(VK_NEXT, false, false, false, true, s) == AppCommand::NextChapter);
    TEST_ASSERT(dispatcher.TranslateKeyChord('P', false, true, false, true, s) == AppCommand::PreviousChapter);

    // Shift+N
    TEST_ASSERT(dispatcher.TranslateKeyChord('N', false, true, false, true, s) == AppCommand::NextChapter);
    // Ctrl+PageDown / Ctrl+PageUp
    TEST_ASSERT(dispatcher.TranslateKeyChord(VK_NEXT, true, false, false, true, s) == AppCommand::NextChapter);
    TEST_ASSERT(dispatcher.TranslateKeyChord(VK_PRIOR, true, false, false, true, s) == AppCommand::PreviousChapter);

    // Key translations when player is closed
    TEST_ASSERT(dispatcher.TranslateKeyChord(VK_NEXT, false, false, false, false, s) == AppCommand::None);
}

TEST_CASE(CommandDispatcher, ChapterNavigationExecutionAndBoundaryHandling) {
    MockCommandContext context;
    CommandDispatcher dispatcher(&context);

    // 1. Player closed -> commands return false
    TEST_ASSERT(!dispatcher.Execute(AppCommand::NextChapter));
    TEST_ASSERT(!dispatcher.Execute(AppCommand::PreviousChapter));

    // 2. Player open with NO chapters -> safe fallback with OSD
    context.session.OpenDirect(L"C:\\media\\test.mp4");
    TEST_ASSERT(context.session.IsOpen());

    context.osdShown = false;
    TEST_ASSERT(dispatcher.Execute(AppCommand::NextChapter));
    TEST_ASSERT(context.osdShown);
    TEST_ASSERT_EQ(context.lastOsdIcon, IconType::Next);
    TEST_ASSERT_EQ(context.lastOsdText, std::wstring(L"No Chapters"));

    context.osdShown = false;
    TEST_ASSERT(dispatcher.Execute(AppCommand::PreviousChapter));
    TEST_ASSERT(context.osdShown);
    TEST_ASSERT_EQ(context.lastOsdIcon, IconType::Prev);
    TEST_ASSERT_EQ(context.lastOsdText, std::wstring(L"No Chapters"));
}
TEST_CASE(CommandDispatcher, TakeScreenshotExecution) {
    MockCommandContext context;
    CommandDispatcher dispatcher(&context);

    // 1. Session closed -> TakeScreenshot returns false, shows OSD warning
    TEST_ASSERT(!dispatcher.Execute(AppCommand::TakeScreenshot));
    TEST_ASSERT(!context.takeScreenshotCalled);
    TEST_ASSERT(context.osdShown);
    TEST_ASSERT_EQ(context.lastOsdIcon, IconType::VolumeMuted);
    TEST_ASSERT(context.lastOsdText.find(L"unavailable") != std::wstring::npos);

    // 2. Session open -> TakeScreenshot delegates to context
    context.osdShown = false;
    context.session.OpenDirect(L"C:\\media\\test.mp4");
    TEST_ASSERT(context.session.IsOpen());

    TEST_ASSERT(dispatcher.Execute(AppCommand::TakeScreenshot));
    TEST_ASSERT(context.takeScreenshotCalled);

    // 3. String command execution
    context.takeScreenshotCalled = false;
    TEST_ASSERT(dispatcher.Execute("takeScreenshot"));
    TEST_ASSERT(context.takeScreenshotCalled);

    context.takeScreenshotCalled = false;
    TEST_ASSERT(dispatcher.Execute("screenshot"));
    TEST_ASSERT(context.takeScreenshotCalled);
}

} // namespace Aperture::Test
