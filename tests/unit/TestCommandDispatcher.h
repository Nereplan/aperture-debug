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

TEST_CASE(CommandDispatcher, KeyChordParsingAndActionPrecedence) {
    MockCommandContext context;
    CommandDispatcher dispatcher(&context);

    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("togglePlay"), AppCommand::TogglePlay);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("takeScreenshot"), AppCommand::TakeScreenshot);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("nextChapter"), AppCommand::NextChapter);
    TEST_ASSERT_EQ(CommandDispatcher::StringFromCommandId(AppCommand::TogglePlay), std::string("togglePlay"));

    ShortcutBinding customPlay;
    customPlay.actionId = "togglePlay";
    customPlay.vkCode = 'Z';
    customPlay.ctrl = true;
    context.settings.shortcuts["togglePlay"] = customPlay;

    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord('Z', true, false, false, true, context.settings), AppCommand::TogglePlay);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_SPACE, false, false, false, true, context.settings), AppCommand::TogglePlay);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_SPACE, false, false, false, false, context.settings), AppCommand::None);

    Settings fallbackSettings = context.settings;
    fallbackSettings.shortcuts.erase("audioDelayMinus");
    fallbackSettings.shortcuts.erase("audioDelayPlus");
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_LEFT, true, false, false, true, fallbackSettings), AppCommand::FrameStepBackward);
    TEST_ASSERT_EQ(dispatcher.TranslateKeyChord(VK_RIGHT, true, false, false, true, fallbackSettings), AppCommand::FrameStepForward);

    TEST_ASSERT(!dispatcher.Execute(AppCommand::TakeScreenshot));
    TEST_ASSERT(!context.takeScreenshotCalled);

    std::wstring filePath = L"C:\\media\\show_ep1.mkv";
    context.session.OpenDirect(filePath, 0.0);
    context.session.OnPlayerOpened(true);
    MediaItem* item = context.library.AddOrUpdateMedia(filePath);
    TEST_ASSERT(item != nullptr);

    context.popupsOpen = true;
    dispatcher.Execute(AppCommand::ClosePlayer);
    TEST_ASSERT(context.popupsDismissed);
    TEST_ASSERT(!context.closePlayerCalled);

    context.session.OpenQueue({ filePath, L"C:\\media\\show_ep2.mkv" }, 0);
    dispatcher.Execute(AppCommand::PlayNext);
    TEST_ASSERT(context.playNextInQueueCalled);
    TEST_ASSERT(!context.playNextLibraryMediaCalled);

    context.session.OpenDirect(filePath, 0.0);
    context.session.OnPlayerOpened(true);
    context.playNextInQueueCalled = false;
    dispatcher.Execute(AppCommand::PlayNext);
    TEST_ASSERT(!context.playNextInQueueCalled);
    TEST_ASSERT(context.playNextLibraryMediaCalled);

    context.settings.volume = 0.5;
    context.player.SetVolume(0.5);
    dispatcher.Execute(AppCommand::VolumeUp);
    TEST_ASSERT_EQ(std::round(context.settings.volume * 100.0), 55.0);

    dispatcher.Execute(AppCommand::SetSpeed, CommandArgs{ 1.5 });
    TEST_ASSERT(item->preferences.has_value());
    TEST_ASSERT_EQ(*item->preferences->playbackSpeed, 1.5);
    TEST_ASSERT(context.librarySaved);

    TEST_ASSERT(dispatcher.Execute(AppCommand::TakeScreenshot));
    TEST_ASSERT(context.takeScreenshotCalled);
}

} // namespace Aperture::Test
