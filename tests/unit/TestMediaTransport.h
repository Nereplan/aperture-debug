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
#include <atomic>
#include <cmath>

namespace Aperture::Test {

// 1. Command Mapping Tests
TEST_CASE(MediaTransport, CommandMapping) {
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
    TEST_ASSERT_EQ(static_cast<int>(MediaTransport::MapButtonToCommand(static_cast<SystemMediaTransportControlsButton>(999))),
                   static_cast<int>(AppCommand::None));

    // TimeSpan Conversions
    TEST_ASSERT(std::abs(MediaTransport::TimeSpanToSeconds(10000000LL) - 1.0) < 0.0001);
    TEST_ASSERT(std::abs(MediaTransport::TimeSpanToSeconds(55000000LL) - 5.5) < 0.0001);
    TEST_ASSERT(std::abs(MediaTransport::TimeSpanToSeconds(0LL) - 0.0) < 0.0001);
    TEST_ASSERT(std::abs(MediaTransport::TimeSpanToSeconds(-100LL) - 0.0) < 0.0001);

    TEST_ASSERT_EQ(MediaTransport::SecondsToTimeSpan(1.0), 10000000LL);
    TEST_ASSERT_EQ(MediaTransport::SecondsToTimeSpan(5.5), 55000000LL);
    TEST_ASSERT_EQ(MediaTransport::SecondsToTimeSpan(0.0), 0LL);
    TEST_ASSERT_EQ(MediaTransport::SecondsToTimeSpan(-2.0), 0LL);
}

// 2. Capability and State Projection Tests
TEST_CASE(MediaTransport, CapabilityAndStateProjection) {
    PlaybackSession session;
    MediaLibrary library;
    Settings settings;

    // Closed session projection
    {
        MediaTransportSnapshot s = MediaTransport::ProjectState(session, library, settings, 0.0, 0.0);
        TEST_ASSERT_EQ(static_cast<int>(s.playbackState), static_cast<int>(PlaybackState::Closed));
        TEST_ASSERT(!s.isPlayEnabled);
        TEST_ASSERT(!s.isPauseEnabled);
        TEST_ASSERT(!s.isStopEnabled);
        TEST_ASSERT(!s.isNextEnabled);
        TEST_ASSERT(!s.isPreviousEnabled);
        TEST_ASSERT(!s.isSeekEnabled);
    }

    // Direct open playing media
    std::wstring mediaPath = L"C:\\Media\\TestVideo.mp4";
    session.OpenDirect(mediaPath, 0.0);
    session.OnPlayerOpened(true);

    {
        MediaTransportSnapshot s = MediaTransport::ProjectState(session, library, settings, 12.5, 120.0);
        TEST_ASSERT_EQ(static_cast<int>(s.playbackState), static_cast<int>(PlaybackState::Playing));
        TEST_ASSERT_EQ(s.title, std::wstring(L"TestVideo"));
        TEST_ASSERT_EQ(s.artist, std::wstring(L"TestVideo.mp4"));
        TEST_ASSERT(std::abs(s.positionSec - 12.5) < 0.001);
        TEST_ASSERT(std::abs(s.durationSec - 120.0) < 0.001);
        TEST_ASSERT(!s.isPlayEnabled);
        TEST_ASSERT(s.isPauseEnabled);
        TEST_ASSERT(s.isStopEnabled);
        TEST_ASSERT(s.isSeekEnabled);
    }

    // Paused state projection
    session.SetPlayState(false);
    {
        MediaTransportSnapshot s = MediaTransport::ProjectState(session, library, settings, 12.5, 120.0);
        TEST_ASSERT_EQ(static_cast<int>(s.playbackState), static_cast<int>(PlaybackState::Paused));
        TEST_ASSERT(s.isPlayEnabled);
        TEST_ASSERT(!s.isPauseEnabled);
        TEST_ASSERT(s.isStopEnabled);
        TEST_ASSERT(s.isSeekEnabled);
    }

    // Media item with metadata in library
    MediaItem* item = library.AddOrUpdateMedia(mediaPath);
    item->title = L"Custom Movie Title";
    item->durationSec = 150.0;
    JsonValue extra = JsonValue::Object();
    extra["artist"] = JsonValue(std::string("Famous Artist"));
    item->extraFields = extra;

    {
        MediaTransportSnapshot s = MediaTransport::ProjectState(session, library, settings, 5.0, 150.0);
        TEST_ASSERT_EQ(s.title, std::wstring(L"Custom Movie Title"));
        TEST_ASSERT_EQ(s.artist, std::wstring(L"Famous Artist"));
        TEST_ASSERT(std::abs(s.durationSec - 150.0) < 0.001);
    }

    // Queue Navigation Capabilities (no loop)
    settings.loop = false;
    std::vector<std::wstring> queue = {
        L"C:\\Media\\track1.mp3",
        L"C:\\Media\\track2.mp3",
        L"C:\\Media\\track3.mp3"
    };
    session.OpenQueue(queue, 0);
    session.OnPlayerOpened(true);

    {
        MediaTransportSnapshot s = MediaTransport::ProjectState(session, library, settings, 0.0, 60.0);
        TEST_ASSERT(s.isNextEnabled);
        TEST_ASSERT(!s.isPreviousEnabled);
    }

    session.OpenQueue(queue, 1);
    {
        MediaTransportSnapshot s = MediaTransport::ProjectState(session, library, settings, 0.0, 60.0);
        TEST_ASSERT(s.isNextEnabled);
        TEST_ASSERT(s.isPreviousEnabled);
    }

    session.OpenQueue(queue, 2);
    {
        MediaTransportSnapshot s = MediaTransport::ProjectState(session, library, settings, 0.0, 60.0);
        TEST_ASSERT(!s.isNextEnabled);
        TEST_ASSERT(s.isPreviousEnabled);
    }

    // Queue Navigation with Loop Enabled
    settings.loop = true;
    {
        MediaTransportSnapshot s = MediaTransport::ProjectState(session, library, settings, 0.0, 60.0);
        TEST_ASSERT(s.isNextEnabled);
        TEST_ASSERT(s.isPreviousEnabled);
    }
}

// 3. Timeline Bounds Tests
TEST_CASE(MediaTransport, TimelineBounds) {
    // Normal in-range position
    {
        MediaTransportTimeline t = MediaTransport::ComputeTimelineProperties(15.0, 100.0);
        TEST_ASSERT(std::abs(t.startTimeSec - 0.0) < 0.0001);
        TEST_ASSERT(std::abs(t.endTimeSec - 100.0) < 0.0001);
        TEST_ASSERT(std::abs(t.minSeekTimeSec - 0.0) < 0.0001);
        TEST_ASSERT(std::abs(t.maxSeekTimeSec - 100.0) < 0.0001);
        TEST_ASSERT(std::abs(t.positionSec - 15.0) < 0.0001);
    }

    // Clamping position beyond duration
    {
        MediaTransportTimeline t = MediaTransport::ComputeTimelineProperties(150.0, 100.0);
        TEST_ASSERT(std::abs(t.positionSec - 100.0) < 0.0001);
        TEST_ASSERT(std::abs(t.endTimeSec - 100.0) < 0.0001);
    }

    // Negative position clamped to zero
    {
        MediaTransportTimeline t = MediaTransport::ComputeTimelineProperties(-10.0, 100.0);
        TEST_ASSERT(std::abs(t.positionSec - 0.0) < 0.0001);
    }

    // Zero duration / Live stream
    {
        MediaTransportTimeline t = MediaTransport::ComputeTimelineProperties(10.0, 0.0);
        TEST_ASSERT(std::abs(t.startTimeSec - 0.0) < 0.0001);
        TEST_ASSERT(std::abs(t.endTimeSec - 0.0) < 0.0001);
        TEST_ASSERT(std::abs(t.positionSec - 0.0) < 0.0001);
    }

    // Negative duration clamped to zero
    {
        MediaTransportTimeline t = MediaTransport::ComputeTimelineProperties(5.0, -50.0);
        TEST_ASSERT(std::abs(t.startTimeSec - 0.0) < 0.0001);
        TEST_ASSERT(std::abs(t.endTimeSec - 0.0) < 0.0001);
        TEST_ASSERT(std::abs(t.positionSec - 0.0) < 0.0001);
    }
}

// 4. Callback Marshaling Contract Tests
TEST_CASE(MediaTransport, CallbackMarshalingContract) {
    MockCommandContext mockCtx;
    CommandDispatcher dispatcher(&mockCtx);
    MediaTransport transport;

    mockCtx.session.OpenDirect(L"C:\\Media\\Test.mp4", 0.0);
    mockCtx.session.OnPlayerOpened(true);

    TEST_ASSERT_EQ(transport.GetPendingActionCount(), 0u);

    // Queue single action
    CommandArgs args;
    args.doubleVal = 45.0;
    transport.QueueAction(AppCommand::SeekTo, args);
    TEST_ASSERT_EQ(transport.GetPendingActionCount(), 1u);

    // Process commands on UI thread
    size_t executed = transport.ProcessPendingCommands(dispatcher);
    TEST_ASSERT_EQ(executed, 1u);
    TEST_ASSERT_EQ(transport.GetPendingActionCount(), 0u);

    // Multi-threaded queuing
    const int threadCount = 4;
    const int actionsPerThread = 25;
    std::vector<std::thread> workers;
    for (int i = 0; i < threadCount; ++i) {
        workers.emplace_back([&transport, actionsPerThread]() {
            for (int j = 0; j < actionsPerThread; ++j) {
                AppCommand cmd = (j % 2 == 0) ? AppCommand::Play : AppCommand::Pause;
                transport.QueueAction(cmd);
            }
        });
    }
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    TEST_ASSERT_EQ(transport.GetPendingActionCount(), static_cast<size_t>(threadCount * actionsPerThread));

    executed = transport.ProcessPendingCommands(dispatcher);
    TEST_ASSERT_EQ(executed, static_cast<size_t>(threadCount * actionsPerThread));
    TEST_ASSERT_EQ(transport.GetPendingActionCount(), 0u);

    // Disabled callbacks contract
    transport.SetCallbacksDisabled(true);
    TEST_ASSERT(transport.AreCallbacksDisabled());
    transport.QueueAction(AppCommand::Play);
    TEST_ASSERT_EQ(transport.GetPendingActionCount(), 0u);

    executed = transport.ProcessPendingCommands(dispatcher);
    TEST_ASSERT_EQ(executed, 0u);
}

// 5. Failed Initialization and Degradation Tests
TEST_CASE(MediaTransport, FailedInitializationAndDegradation) {
    MediaTransport transport;

    // NULL HWND initialization failure
    bool ok = transport.Initialize(NULL);
    TEST_ASSERT(!ok);
    TEST_ASSERT(!transport.IsInitialized());
    TEST_ASSERT(!transport.IsEnabled());

    // Invalid window handle
    HWND fakeHwnd = reinterpret_cast<HWND>(static_cast<INT_PTR>(0xDEADBEEF));
    ok = transport.Initialize(fakeHwnd);
    TEST_ASSERT(!ok);
    TEST_ASSERT(!transport.IsInitialized());

    // Safe operations on uninitialized transport
    MediaTransportSnapshot s;
    s.title = L"Uninitialized Track";
    s.durationSec = 60.0;
    transport.Update(s, true);
    TEST_ASSERT_EQ(transport.GetLastSnapshot().title, std::wstring(L"Uninitialized Track"));

    transport.Clear();
    TEST_ASSERT_EQ(static_cast<int>(transport.GetLastSnapshot().playbackState), static_cast<int>(PlaybackState::Closed));

    MockCommandContext mockCtx;
    CommandDispatcher dispatcher(&mockCtx);
    size_t processed = transport.ProcessPendingCommands(dispatcher);
    TEST_ASSERT_EQ(processed, 0u);
}

// 6. Clean Teardown and Lifecycle Tests
TEST_CASE(MediaTransport, CleanTeardownAndLifecycle) {
    MediaTransport transport;

    // Teardown uninitialized transport
    transport.Shutdown();
    TEST_ASSERT(!transport.IsInitialized());
    TEST_ASSERT(!transport.IsEnabled());
    TEST_ASSERT_EQ(transport.GetPendingActionCount(), 0u);

    // Queue action then shutdown
    transport.QueueAction(AppCommand::Play);
    transport.Shutdown();
    TEST_ASSERT_EQ(transport.GetPendingActionCount(), 0u);
    TEST_ASSERT(transport.AreCallbacksDisabled());

    // Idempotent Shutdown calls
    transport.Shutdown();
    transport.Shutdown();
    TEST_ASSERT(!transport.IsInitialized());

    // Clear restores closed snapshot state
    MediaTransportSnapshot s;
    s.playbackState = PlaybackState::Playing;
    s.title = L"Playing Song";
    s.isPlayEnabled = false;
    s.isPauseEnabled = true;
    transport.Update(s, true);

    transport.Clear();
    TEST_ASSERT_EQ(static_cast<int>(transport.GetLastSnapshot().playbackState), static_cast<int>(PlaybackState::Closed));
    TEST_ASSERT(!transport.GetLastSnapshot().isPlayEnabled);
    TEST_ASSERT(!transport.GetLastSnapshot().isPauseEnabled);
}

} // namespace Aperture::Test
