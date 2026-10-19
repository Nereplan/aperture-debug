#pragma once

#include "../support/TestFramework.h"
#include "TestCommandDispatcher.h"
#include "../../src/MediaTransport.h"
#include "../../src/PlaybackSession.h"
#include "../../src/CommandDispatcher.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Settings.h"
#include "../../src/MpvPlayer.h"

#include <windows.h>
#include <thread>
#include <vector>
#include <cmath>

namespace Aperture::Test {

TEST_CASE(MediaTransport, SmtcProjectionAndLifecycle) {
    using namespace ABI::Windows::Media;

    TEST_ASSERT_EQ(static_cast<int>(MediaTransport::MapButtonToCommand(SystemMediaTransportControlsButton_Play)),
                   static_cast<int>(AppCommand::Play));
    TEST_ASSERT_EQ(static_cast<int>(MediaTransport::MapButtonToCommand(SystemMediaTransportControlsButton_Pause)),
                   static_cast<int>(AppCommand::Pause));
    TEST_ASSERT_EQ(static_cast<int>(MediaTransport::MapButtonToCommand(SystemMediaTransportControlsButton_Stop)),
                   static_cast<int>(AppCommand::ClosePlayer));
    TEST_ASSERT_EQ(static_cast<int>(MediaTransport::MapButtonToCommand(SystemMediaTransportControlsButton_Next)),
                   static_cast<int>(AppCommand::PlayNext));
    TEST_ASSERT_EQ(static_cast<int>(MediaTransport::MapButtonToCommand(SystemMediaTransportControlsButton_Previous)),
                   static_cast<int>(AppCommand::PlayPrevious));
    TEST_ASSERT_EQ(static_cast<int>(MediaTransport::MapButtonToCommand(SystemMediaTransportControlsButton_Record)),
                   static_cast<int>(AppCommand::None));

    TEST_ASSERT(std::abs(MediaTransport::TimeSpanToSeconds(55000000LL) - 5.5) < 0.0001);
    TEST_ASSERT(std::abs(MediaTransport::TimeSpanToSeconds(-100LL) - 0.0) < 0.0001);
    TEST_ASSERT_EQ(MediaTransport::SecondsToTimeSpan(5.5), 55000000LL);
    TEST_ASSERT_EQ(MediaTransport::SecondsToTimeSpan(-2.0), 0LL);

    PlaybackSession session;
    MediaLibrary library;
    Settings settings;

    MediaTransportSnapshot closed = MediaTransport::ProjectState(session, library, settings, 0.0, 0.0);
    TEST_ASSERT_EQ(static_cast<int>(closed.playbackState), static_cast<int>(PlaybackState::Closed));
    TEST_ASSERT(!closed.isPlayEnabled && !closed.isPauseEnabled && !closed.isStopEnabled && !closed.isSeekEnabled);

    std::wstring mediaPath = L"C:\\Media\\TestVideo.mp4";
    session.OpenDirect(mediaPath, 0.0);
    session.OnPlayerOpened(true);

    MediaTransportSnapshot playing = MediaTransport::ProjectState(session, library, settings, 12.5, 120.0);
    TEST_ASSERT_EQ(static_cast<int>(playing.playbackState), static_cast<int>(PlaybackState::Playing));
    TEST_ASSERT_EQ(playing.title, std::wstring(L"TestVideo"));
    TEST_ASSERT(!playing.isPlayEnabled && playing.isPauseEnabled && playing.isStopEnabled && playing.isSeekEnabled);

    session.SetPlayState(false);
    MediaTransportSnapshot paused = MediaTransport::ProjectState(session, library, settings, 12.5, 120.0);
    TEST_ASSERT_EQ(static_cast<int>(paused.playbackState), static_cast<int>(PlaybackState::Paused));
    TEST_ASSERT(paused.isPlayEnabled && !paused.isPauseEnabled);

    MediaItem* item = library.AddOrUpdateMedia(mediaPath);
    item->title = L"Custom Movie Title";
    item->durationSec = 150.0;
    JsonValue extra = JsonValue::Object();
    extra["artist"] = JsonValue(std::string("Famous Artist"));
    item->extraFields = extra;

    MediaTransportSnapshot enriched = MediaTransport::ProjectState(session, library, settings, 5.0, 150.0);
    TEST_ASSERT_EQ(enriched.title, std::wstring(L"Custom Movie Title"));
    TEST_ASSERT_EQ(enriched.artist, std::wstring(L"Famous Artist"));

    settings.loop = false;
    std::vector<std::wstring> queue = { L"C:\\Media\\track1.mp3", L"C:\\Media\\track2.mp3", L"C:\\Media\\track3.mp3" };
    session.OpenQueue(queue, 0);
    session.OnPlayerOpened(true);
    MediaTransportSnapshot q0 = MediaTransport::ProjectState(session, library, settings, 0.0, 60.0);
    TEST_ASSERT(q0.isNextEnabled && !q0.isPreviousEnabled);

    session.OpenQueue(queue, 2);
    MediaTransportSnapshot q2 = MediaTransport::ProjectState(session, library, settings, 0.0, 60.0);
    TEST_ASSERT(!q2.isNextEnabled && q2.isPreviousEnabled);

    settings.loop = true;
    MediaTransportSnapshot qLoop = MediaTransport::ProjectState(session, library, settings, 0.0, 60.0);
    TEST_ASSERT(qLoop.isNextEnabled && qLoop.isPreviousEnabled);

    MediaTransportTimeline tNormal = MediaTransport::ComputeTimelineProperties(15.0, 100.0);
    TEST_ASSERT(std::abs(tNormal.positionSec - 15.0) < 0.0001 && std::abs(tNormal.endTimeSec - 100.0) < 0.0001);
    TEST_ASSERT(std::abs(MediaTransport::ComputeTimelineProperties(150.0, 100.0).positionSec - 100.0) < 0.0001);
    TEST_ASSERT(std::abs(MediaTransport::ComputeTimelineProperties(-10.0, 100.0).positionSec - 0.0) < 0.0001);
    TEST_ASSERT(std::abs(MediaTransport::ComputeTimelineProperties(10.0, 0.0).endTimeSec - 0.0) < 0.0001);

    MockCommandContext mockCtx;
    CommandDispatcher dispatcher(&mockCtx);
    MediaTransport transport;

    TEST_ASSERT(!transport.Initialize(NULL));
    TEST_ASSERT(!transport.Initialize(reinterpret_cast<HWND>(static_cast<INT_PTR>(0xDEADBEEF))));
    TEST_ASSERT(!transport.IsInitialized());
    transport.SetCallbacksDisabled(false);

    mockCtx.session.OpenDirect(L"C:\\Media\\Test.mp4", 0.0);
    mockCtx.session.OnPlayerOpened(true);

    CommandArgs args;
    args.doubleVal = 45.0;
    transport.QueueAction(AppCommand::SeekTo, args);
    TEST_ASSERT_EQ(transport.GetPendingActionCount(), 1u);
    TEST_ASSERT_EQ(transport.ProcessPendingCommands(dispatcher), 1u);
    TEST_ASSERT_EQ(transport.GetPendingActionCount(), 0u);

    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&transport]() {
            for (int j = 0; j < 25; ++j) {
                transport.QueueAction((j % 2 == 0) ? AppCommand::Play : AppCommand::Pause);
            }
        });
    }
    for (auto& w : workers) w.join();
    TEST_ASSERT_EQ(transport.GetPendingActionCount(), 100u);
    TEST_ASSERT_EQ(transport.ProcessPendingCommands(dispatcher), 100u);

    transport.SetCallbacksDisabled(true);
    transport.QueueAction(AppCommand::Play);
    TEST_ASSERT_EQ(transport.GetPendingActionCount(), 0u);

    transport.SetCallbacksDisabled(false);
    transport.QueueAction(AppCommand::Play);
    transport.Shutdown();
    TEST_ASSERT_EQ(transport.GetPendingActionCount(), 0u);
    TEST_ASSERT(transport.AreCallbacksDisabled());
}

} // namespace Aperture::Test
