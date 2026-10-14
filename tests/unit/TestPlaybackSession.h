#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Json.h"
#include "../../src/PlaybackSession.h"
#include "../../src/Settings.h"
#include "../../src/WindowGeometry.h"
#include <commctrl.h>

namespace Aperture::Test {

TEST_CASE(PlaybackSession, InitialState) {
    PlaybackSession session;
    TEST_ASSERT_EQ(session.GetState(), PlaybackState::Closed);
    TEST_ASSERT_EQ(session.GetPresentationMode(), PresentationMode::Windowed);
    TEST_ASSERT_EQ(session.GetQueueMode(), QueueMode::DirectOpen);
    TEST_ASSERT(session.IsClosed());
    TEST_ASSERT(!session.IsOpen());
    TEST_ASSERT(!session.IsPlaying());
    TEST_ASSERT(!session.IsPaused());
    TEST_ASSERT(!session.IsError());
    TEST_ASSERT(!session.IsFullscreen());
    TEST_ASSERT(!session.IsPiP());
    TEST_ASSERT(session.IsWindowed());
    TEST_ASSERT(!session.IsQueuePlayback());
    TEST_ASSERT(session.GetQueue().empty());
    TEST_ASSERT_EQ(session.GetQueueIndex(), 0ull);
    TEST_ASSERT(session.GetCurrentFilePath().empty());
}

TEST_CASE(PlaybackSession, DirectOpenTransitions) {
    PlaybackSession session;

    // Direct open transitions to Initializing and clears queue
    bool openRes = session.OpenDirect(L"C:\\media\\video1.mp4", 12.5);
    TEST_ASSERT(openRes);
    TEST_ASSERT_EQ(session.GetState(), PlaybackState::Initializing);
    TEST_ASSERT(session.IsOpen());
    TEST_ASSERT(session.IsInitializing());
    TEST_ASSERT(!session.IsQueuePlayback());
    TEST_ASSERT_EQ(session.GetQueueMode(), QueueMode::DirectOpen);
    TEST_ASSERT(session.GetQueue().empty());
    TEST_ASSERT_EQ(session.GetCurrentFilePath(), std::wstring(L"C:\\media\\video1.mp4"));
    TEST_ASSERT_EQ(session.GetStartPos(), 12.5);

    // Player open success transitions to Playing
    bool playerOpened = session.OnPlayerOpened(true);
    TEST_ASSERT(playerOpened);
    TEST_ASSERT_EQ(session.GetState(), PlaybackState::Playing);
    TEST_ASSERT(session.IsPlaying());
    TEST_ASSERT(!session.IsPaused());

    // Pause transition
    bool pauseRes = session.SetPlayState(false);
    TEST_ASSERT(pauseRes);
    TEST_ASSERT_EQ(session.GetState(), PlaybackState::Paused);
    TEST_ASSERT(session.IsPaused());
    TEST_ASSERT(!session.IsPlaying());

    // Resume transition
    bool resumeRes = session.SetPlayState(true);
    TEST_ASSERT(resumeRes);
    TEST_ASSERT_EQ(session.GetState(), PlaybackState::Playing);
    TEST_ASSERT(session.IsPlaying());

    // Error transition
    bool errRes = session.OnPlayerError(L"Decoding failed");
    TEST_ASSERT(errRes);
    TEST_ASSERT_EQ(session.GetState(), PlaybackState::Error);
    TEST_ASSERT(session.IsError());
    TEST_ASSERT(!session.IsOpen());
    TEST_ASSERT_EQ(session.GetCurrentError(), std::wstring(L"Decoding failed"));

    // Close transition resets to Closed state
    session.Close();
    TEST_ASSERT_EQ(session.GetState(), PlaybackState::Closed);
    TEST_ASSERT(session.IsClosed());
    TEST_ASSERT(session.GetCurrentFilePath().empty());
    TEST_ASSERT(session.GetCurrentError().empty());
}

TEST_CASE(PlaybackSession, QueueOpenAndNavigation) {
    PlaybackSession session;

    std::vector<std::wstring> playlist = {
        L"C:\\media\\ep1.mp4",
        L"C:\\media\\ep2.mp4",
        L"C:\\media\\ep3.mp4"
    };

    // Open queue starting at index 1
    bool openRes = session.OpenQueue(playlist, 1);
    TEST_ASSERT(openRes);
    TEST_ASSERT(session.IsQueuePlayback());
    TEST_ASSERT_EQ(session.GetQueueMode(), QueueMode::Queue);
    TEST_ASSERT_EQ(session.GetQueueSize(), 3ull);
    TEST_ASSERT_EQ(session.GetQueueIndex(), 1ull);
    TEST_ASSERT_EQ(session.GetCurrentFilePath(), std::wstring(L"C:\\media\\ep2.mp4"));

    session.OnPlayerOpened(true);
    TEST_ASSERT(session.IsPlaying());

    // Next in queue advances to index 2
    std::wstring next1 = session.GetNextQueuePath();
    TEST_ASSERT_EQ(next1, std::wstring(L"C:\\media\\ep3.mp4"));
    TEST_ASSERT_EQ(session.GetQueueIndex(), 2ull);

    // Next at end wraps to index 0
    std::wstring next2 = session.GetNextQueuePath();
    TEST_ASSERT_EQ(next2, std::wstring(L"C:\\media\\ep1.mp4"));
    TEST_ASSERT_EQ(session.GetQueueIndex(), 0ull);

    // Prev without loop at start stays at index 0 (deterministic restart)
    std::wstring prevNoLoop = session.GetPrevQueuePath(false);
    TEST_ASSERT_EQ(prevNoLoop, std::wstring(L"C:\\media\\ep1.mp4"));
    TEST_ASSERT_EQ(session.GetQueueIndex(), 0ull);

    // Prev with loop at start wraps to index 2
    std::wstring prevLoop = session.GetPrevQueuePath(true);
    TEST_ASSERT_EQ(prevLoop, std::wstring(L"C:\\media\\ep3.mp4"));
    TEST_ASSERT_EQ(session.GetQueueIndex(), 2ull);

    // First in queue resets index to 0
    std::wstring first = session.GetFirstQueuePath();
    TEST_ASSERT_EQ(first, std::wstring(L"C:\\media\\ep1.mp4"));
    TEST_ASSERT_EQ(session.GetQueueIndex(), 0ull);

    // Direct open clears queue
    session.OpenDirect(L"C:\\media\\standalone.mp4");
    TEST_ASSERT(!session.IsQueuePlayback());
    TEST_ASSERT(session.GetQueue().empty());
    TEST_ASSERT_EQ(session.GetQueueIndex(), 0ull);
}

TEST_CASE(PlaybackSession, PreserveQueueLogic) {
    PlaybackSession session;

    std::vector<std::wstring> playlist = {
        L"C:\\media\\ep1.mp4",
        L"C:\\media\\ep2.mp4",
        L"C:\\media\\ep3.mp4"
    };

    session.OpenQueue(playlist, 0);
    TEST_ASSERT(session.IsQueuePlayback());
    TEST_ASSERT_EQ(session.GetQueueIndex(), 0ull);

    // Preserving queue with existing item updates queue index
    bool preserveHit = session.OpenPreserveQueue(L"C:\\media\\ep3.mp4", 5.0);
    TEST_ASSERT(preserveHit);
    TEST_ASSERT(session.IsQueuePlayback());
    TEST_ASSERT_EQ(session.GetQueueIndex(), 2ull);
    TEST_ASSERT_EQ(session.GetCurrentFilePath(), std::wstring(L"C:\\media\\ep3.mp4"));

    // Preserving queue with item NOT in queue fails closed to DirectOpen
    bool preserveMiss = session.OpenPreserveQueue(L"C:\\media\\other.mp4", 0.0);
    TEST_ASSERT(preserveMiss);
    TEST_ASSERT(!session.IsQueuePlayback());
    TEST_ASSERT(session.GetQueue().empty());
    TEST_ASSERT_EQ(session.GetQueueIndex(), 0ull);
    TEST_ASSERT_EQ(session.GetCurrentFilePath(), std::wstring(L"C:\\media\\other.mp4"));
}

TEST_CASE(PlaybackSession, EofAndAutoplayTransitions) {
    PlaybackSession session;

    std::vector<std::wstring> playlist = {
        L"C:\\media\\ep1.mp4",
        L"C:\\media\\ep2.mp4"
    };

    session.OpenQueue(playlist, 0);
    session.OnPlayerOpened(true);

    // In queue mode, EOF advances queue regardless of autoplay setting
    std::wstring nextPath;
    bool isQueue = false;
    bool eofQueue = session.OnEof(false, nextPath, isQueue);
    TEST_ASSERT(eofQueue);
    TEST_ASSERT(isQueue);
    TEST_ASSERT_EQ(nextPath, std::wstring(L"C:\\media\\ep2.mp4"));
    TEST_ASSERT_EQ(session.GetQueueIndex(), 1ull);

    // In direct-open mode with autoplay enabled, EOF signals library navigation
    session.OpenDirect(L"C:\\media\\movie.mp4");
    session.OnPlayerOpened(true);
    bool eofAutoplay = session.OnEof(true, nextPath, isQueue);
    TEST_ASSERT(eofAutoplay);
    TEST_ASSERT(!isQueue);
    TEST_ASSERT(nextPath.empty());

    // In direct-open mode with autoplay disabled, EOF transitions to Paused
    bool eofNoAutoplay = session.OnEof(false, nextPath, isQueue);
    TEST_ASSERT(!eofNoAutoplay);
    TEST_ASSERT(session.IsPaused());
}

TEST_CASE(PlaybackSession, FourItemQueueEofProgressionWithoutSkips) {
    PlaybackSession session;

    std::vector<std::wstring> playlist = {
        L"C:\\media\\item1.mp4",
        L"C:\\media\\item2.mp4",
        L"C:\\media\\item3.mp4",
        L"C:\\media\\item4.mp4"
    };

    // Open queue starting at index 0 (item1)
    bool openRes = session.OpenQueue(playlist, 0);
    TEST_ASSERT(openRes);
    TEST_ASSERT(session.IsQueuePlayback());
    TEST_ASSERT_EQ(session.GetQueueIndex(), 0ull);
    TEST_ASSERT_EQ(session.GetCurrentFilePath(), std::wstring(L"C:\\media\\item1.mp4"));
    session.OnPlayerOpened(true);
    TEST_ASSERT(session.IsPlaying());

    // EOF 1: advances exactly once to item2 (index 1)
    std::wstring nextPath;
    bool isQueue = false;
    bool eof1 = session.OnEof(false, nextPath, isQueue);
    TEST_ASSERT(eof1);
    TEST_ASSERT(isQueue);
    TEST_ASSERT_EQ(nextPath, std::wstring(L"C:\\media\\item2.mp4"));
    TEST_ASSERT_EQ(session.GetQueueIndex(), 1ull);

    // Simulated playback loop: open the selected path
    bool opened2 = session.OpenPreserveQueue(nextPath, 0.0);
    TEST_ASSERT(opened2);
    TEST_ASSERT_EQ(session.GetQueueIndex(), 1ull);
    TEST_ASSERT_EQ(session.GetCurrentFilePath(), std::wstring(L"C:\\media\\item2.mp4"));
    session.OnPlayerOpened(true);

    // EOF 2: advances exactly once to item3 (index 2)
    bool eof2 = session.OnEof(false, nextPath, isQueue);
    TEST_ASSERT(eof2);
    TEST_ASSERT(isQueue);
    TEST_ASSERT_EQ(nextPath, std::wstring(L"C:\\media\\item3.mp4"));
    TEST_ASSERT_EQ(session.GetQueueIndex(), 2ull);

    bool opened3 = session.OpenPreserveQueue(nextPath, 0.0);
    TEST_ASSERT(opened3);
    TEST_ASSERT_EQ(session.GetQueueIndex(), 2ull);
    TEST_ASSERT_EQ(session.GetCurrentFilePath(), std::wstring(L"C:\\media\\item3.mp4"));
    session.OnPlayerOpened(true);

    // EOF 3: advances exactly once to item4 (index 3)
    bool eof3 = session.OnEof(false, nextPath, isQueue);
    TEST_ASSERT(eof3);
    TEST_ASSERT(isQueue);
    TEST_ASSERT_EQ(nextPath, std::wstring(L"C:\\media\\item4.mp4"));
    TEST_ASSERT_EQ(session.GetQueueIndex(), 3ull);

    bool opened4 = session.OpenPreserveQueue(nextPath, 0.0);
    TEST_ASSERT(opened4);
    TEST_ASSERT_EQ(session.GetQueueIndex(), 3ull);
    TEST_ASSERT_EQ(session.GetCurrentFilePath(), std::wstring(L"C:\\media\\item4.mp4"));
    session.OnPlayerOpened(true);

    // EOF 4: wraps around to item1 (index 0)
    bool eof4 = session.OnEof(false, nextPath, isQueue);
    TEST_ASSERT(eof4);
    TEST_ASSERT(isQueue);
    TEST_ASSERT_EQ(nextPath, std::wstring(L"C:\\media\\item1.mp4"));
    TEST_ASSERT_EQ(session.GetQueueIndex(), 0ull);

    bool opened1 = session.OpenPreserveQueue(nextPath, 0.0);
    TEST_ASSERT(opened1);
    TEST_ASSERT_EQ(session.GetQueueIndex(), 0ull);
    TEST_ASSERT_EQ(session.GetCurrentFilePath(), std::wstring(L"C:\\media\\item1.mp4"));
    session.OnPlayerOpened(true);

    // Verify manual Next advances once independently
    std::wstring manualNext = session.GetNextQueuePath();
    TEST_ASSERT_EQ(manualNext, std::wstring(L"C:\\media\\item2.mp4"));
    TEST_ASSERT_EQ(session.GetQueueIndex(), 1ull);

    // Manual Prev with loop=false at index 1 -> index 0
    std::wstring manualPrev = session.GetPrevQueuePath(false);
    TEST_ASSERT_EQ(manualPrev, std::wstring(L"C:\\media\\item1.mp4"));
    TEST_ASSERT_EQ(session.GetQueueIndex(), 0ull);

    // Manual Prev with loop=true at index 0 -> wraps to index 3 (item4)
    std::wstring manualPrevWrap = session.GetPrevQueuePath(true);
    TEST_ASSERT_EQ(manualPrevWrap, std::wstring(L"C:\\media\\item4.mp4"));
    TEST_ASSERT_EQ(session.GetQueueIndex(), 3ull);
}

TEST_CASE(PlaybackSession, TrackStateAndUnmutedVolume) {
    PlaybackSession session;

    session.SetLastActiveSubtitleTrackId(2);
    session.SetLastActiveSubtitleTrackTitle(L"English [SDH]");
    TEST_ASSERT_EQ(session.GetLastActiveSubtitleTrackId(), 2);
    TEST_ASSERT_EQ(session.GetLastActiveSubtitleTrackTitle(), std::wstring(L"English [SDH]"));

    session.SetLastActiveAudioTrackId(3);
    session.SetLastActiveAudioTrackTitle(L"Japanese (5.1)");
    TEST_ASSERT_EQ(session.GetLastActiveAudioTrackId(), 3);
    TEST_ASSERT_EQ(session.GetLastActiveAudioTrackTitle(), std::wstring(L"Japanese (5.1)"));

    // Prior unmuted volume tracking
    session.SetPriorUnmutedVolume(0.85);
    TEST_ASSERT_EQ(session.GetPriorUnmutedVolume(), 0.85);

    // Below threshold does not overwrite prior unmuted volume
    session.SetPriorUnmutedVolume(0.005);
    TEST_ASSERT_EQ(session.GetPriorUnmutedVolume(), 0.85);

    // Updating media path references
    session.OpenQueue({ L"C:\\media\\old_name.mp4", L"C:\\media\\other.mp4" }, 0);
    bool pathChanged = session.UpdatePathReferences(L"C:\\media\\old_name.mp4", L"D:\\new_media\\renamed.mp4");
    TEST_ASSERT(pathChanged);
    TEST_ASSERT_EQ(session.GetCurrentFilePath(), std::wstring(L"D:\\new_media\\renamed.mp4"));
    TEST_ASSERT_EQ(session.GetQueue()[0], std::wstring(L"D:\\new_media\\renamed.mp4"));
}

TEST_CASE(PlaybackSession, QueueIndexPositionRoundTrip) {
    ScopedTempDir tempDir(L"Aperture_Session_RoundTrip_");
    std::wstring f1 = tempDir.SubPath(L"ep1.mp4");
    std::wstring f2 = tempDir.SubPath(L"ep2.mp4");
    std::wstring f3 = tempDir.SubPath(L"ep3.mp4");
    tempDir.WriteFile(L"ep1.mp4", "dummy video 1");
    tempDir.WriteFile(L"ep2.mp4", "dummy video 2");
    tempDir.WriteFile(L"ep3.mp4", "dummy video 3");

    std::wstring sessionPath = tempDir.SubPath(L"session.json");

    PlaybackSession session;
    session.OpenQueue({ f1, f2, f3 }, 1);
    session.SetPlaybackPosition(124.75);

    StorageStatus saveRes = session.Save(sessionPath, tempDir.GetPath());
    TEST_ASSERT(saveRes.is_ok());
    TEST_ASSERT(FileExists(sessionPath));

    PlaybackSession loadedSession;
    StorageStatus loadRes = loadedSession.Load(sessionPath, tempDir.GetPath());
    TEST_ASSERT(loadRes.is_ok());
    TEST_ASSERT_EQ(loadedSession.GetQueueMode(), QueueMode::Queue);
    TEST_ASSERT(loadedSession.IsQueuePlayback());
    TEST_ASSERT_EQ(loadedSession.GetQueueSize(), 3ull);
    TEST_ASSERT_EQ(loadedSession.GetQueue()[0], f1);
    TEST_ASSERT_EQ(loadedSession.GetQueue()[1], f2);
    TEST_ASSERT_EQ(loadedSession.GetQueue()[2], f3);
    TEST_ASSERT_EQ(loadedSession.GetQueueIndex(), 1ull);
    TEST_ASSERT_EQ(loadedSession.GetCurrentFilePath(), f2);
    TEST_ASSERT_EQ(loadedSession.GetPlaybackPosition(), 124.75);
    TEST_ASSERT_EQ(loadedSession.GetStartPos(), 124.75);
    TEST_ASSERT_EQ(loadedSession.GetState(), PlaybackState::Closed);
    TEST_ASSERT(!loadedSession.IsOpen());
}

TEST_CASE(PlaybackSession, MissingEntryRecovery) {
    ScopedTempDir tempDir(L"Aperture_Session_Missing_");
    std::wstring f1 = tempDir.SubPath(L"ep1.mp4");
    std::wstring f2 = tempDir.SubPath(L"ep2.mp4");
    std::wstring f3 = tempDir.SubPath(L"ep3.mp4");
    tempDir.WriteFile(L"ep1.mp4", "dummy 1");
    tempDir.WriteFile(L"ep2.mp4", "dummy 2");
    tempDir.WriteFile(L"ep3.mp4", "dummy 3");

    std::wstring sessionPath = tempDir.SubPath(L"session.json");

    // Queue of [f1, f2, f3], current index = 2 (f3)
    PlaybackSession session;
    session.OpenQueue({ f1, f2, f3 }, 2);
    session.SetPlaybackPosition(45.0);
    TEST_ASSERT(session.Save(sessionPath, tempDir.GetPath()).is_ok());

    // Delete f2 (before current index)
    DeleteFileW(f2.c_str());

    PlaybackSession s1;
    TEST_ASSERT(s1.Load(sessionPath, tempDir.GetPath()).is_ok());
    TEST_ASSERT_EQ(s1.GetQueueSize(), 2ull);
    TEST_ASSERT_EQ(s1.GetQueue()[0], f1);
    TEST_ASSERT_EQ(s1.GetQueue()[1], f3);
    // Index was 2, now corrected to 1 because f2 was removed before it
    TEST_ASSERT_EQ(s1.GetQueueIndex(), 1ull);
    TEST_ASSERT_EQ(s1.GetCurrentFilePath(), f3);
    TEST_ASSERT_EQ(s1.GetPlaybackPosition(), 45.0);

    // Now delete f3 (the current file itself)
    DeleteFileW(f3.c_str());

    PlaybackSession s2;
    TEST_ASSERT(s2.Load(sessionPath, tempDir.GetPath()).is_ok());
    TEST_ASSERT_EQ(s2.GetQueueSize(), 1ull);
    TEST_ASSERT_EQ(s2.GetQueue()[0], f1);
    // Index clamped to 0
    TEST_ASSERT_EQ(s2.GetQueueIndex(), 0ull);
    TEST_ASSERT_EQ(s2.GetCurrentFilePath(), f1);

    // Now delete f1 as well (all files missing)
    DeleteFileW(f1.c_str());

    PlaybackSession s3;
    TEST_ASSERT(s3.Load(sessionPath, tempDir.GetPath()).is_ok());
    TEST_ASSERT_EQ(s3.GetQueueSize(), 0ull);
    TEST_ASSERT(s3.GetQueue().empty());
    TEST_ASSERT_EQ(s3.GetQueueIndex(), 0ull);
    TEST_ASSERT(s3.GetCurrentFilePath().empty());
    TEST_ASSERT_EQ(s3.GetPlaybackPosition(), 0.0);
    TEST_ASSERT_EQ(s3.GetState(), PlaybackState::Closed);
    TEST_ASSERT_EQ(s3.GetQueueMode(), QueueMode::DirectOpen);
}

TEST_CASE(PlaybackSession, DirectMediaState) {
    ScopedTempDir tempDir(L"Aperture_Session_Direct_");
    std::wstring directFile = tempDir.SubPath(L"single_movie.mp4");
    tempDir.WriteFile(L"single_movie.mp4", "movie data");

    std::wstring sessionPath = tempDir.SubPath(L"session.json");

    PlaybackSession session;
    session.OpenDirect(directFile, 88.5);
    TEST_ASSERT_EQ(session.GetPlaybackPosition(), 88.5);
    TEST_ASSERT(session.Save(sessionPath, tempDir.GetPath()).is_ok());

    // Verify stored JSON has direct mode
    std::string jsonText;
    TEST_ASSERT(ReadTextFile(sessionPath, jsonText).is_ok());
    JsonValue root;
    TEST_ASSERT(JsonValue::parse(jsonText, root));
    TEST_ASSERT_EQ(root.get("queueMode").as_string(), "direct");
    TEST_ASSERT_EQ(root.get("playbackPositionSec").as_number(), 88.5);
    TEST_ASSERT(root.get("queue").is_array());
    TEST_ASSERT_EQ(root.get("queue").as_array().size(), 0ull);

    PlaybackSession loadedSession;
    TEST_ASSERT(loadedSession.Load(sessionPath, tempDir.GetPath()).is_ok());
    TEST_ASSERT_EQ(loadedSession.GetQueueMode(), QueueMode::DirectOpen);
    TEST_ASSERT(!loadedSession.IsQueuePlayback());
    TEST_ASSERT_EQ(loadedSession.GetCurrentFilePath(), directFile);
    TEST_ASSERT_EQ(loadedSession.GetPlaybackPosition(), 88.5);
    TEST_ASSERT_EQ(loadedSession.GetStartPos(), 88.5);
    TEST_ASSERT(loadedSession.GetQueue().empty());

    // Delete single file and verify recovery
    DeleteFileW(directFile.c_str());
    PlaybackSession sMissing;
    TEST_ASSERT(sMissing.Load(sessionPath, tempDir.GetPath()).is_ok());
    TEST_ASSERT(sMissing.GetCurrentFilePath().empty());
    TEST_ASSERT_EQ(sMissing.GetPlaybackPosition(), 0.0);
    TEST_ASSERT_EQ(sMissing.GetQueueMode(), QueueMode::DirectOpen);
    TEST_ASSERT_EQ(sMissing.GetState(), PlaybackState::Closed);
}

TEST_CASE(PlaybackSession, BoundsAndValidation) {
    ScopedTempDir tempDir(L"Aperture_Session_Bounds_");
    std::wstring mediaFile = tempDir.SubPath(L"item.mp4");
    tempDir.WriteFile(L"item.mp4", "data");
    std::wstring sessionPath = tempDir.SubPath(L"session.json");

    // 1. Negative & NaN / Inf position clamped to 0.0
    {
        JsonValue j = JsonValue::Object();
        j["schemaVersion"] = 1;
        j["queueMode"] = "direct";
        j["currentFilePath"] = Utf16ToUtf8(mediaFile);
        j["playbackPositionSec"] = -100.5;
        j["queue"] = JsonValue::Array();
        tempDir.WriteFile(L"session.json", j.stringify(true));

        PlaybackSession s;
        TEST_ASSERT(s.Load(sessionPath, tempDir.GetPath()).is_ok());
        TEST_ASSERT_EQ(s.GetPlaybackPosition(), 0.0);
        TEST_ASSERT_EQ(s.GetStartPos(), 0.0);
    }

    // 2. Out-of-bounds currentQueueIndex clamped
    {
        JsonValue j = JsonValue::Object();
        j["schemaVersion"] = 1;
        j["queueMode"] = "queue";
        j["currentFilePath"] = Utf16ToUtf8(mediaFile);
        j["currentQueueIndex"] = 999999;
        JsonValue q = JsonValue::Array();
        q.push_back(JsonValue(Utf16ToUtf8(mediaFile)));
        j["queue"] = q;
        tempDir.WriteFile(L"session.json", j.stringify(true));

        PlaybackSession s;
        TEST_ASSERT(s.Load(sessionPath, tempDir.GetPath()).is_ok());
        TEST_ASSERT_EQ(s.GetQueueIndex(), 0ull);
        TEST_ASSERT_EQ(s.GetCurrentFilePath(), mediaFile);
    }

    // 3. Queue size bounded to kMaxQueueSize
    {
        JsonValue j = JsonValue::Object();
        j["schemaVersion"] = 1;
        j["queueMode"] = "queue";
        j["currentFilePath"] = Utf16ToUtf8(mediaFile);
        j["currentQueueIndex"] = 0;
        JsonValue q = JsonValue::Array();
        for (int i = 0; i < 1100; ++i) {
            q.push_back(JsonValue(Utf16ToUtf8(mediaFile)));
        }
        j["queue"] = q;
        tempDir.WriteFile(L"session.json", j.stringify(true));

        PlaybackSession s;
        TEST_ASSERT(s.Load(sessionPath, tempDir.GetPath()).is_ok());
        TEST_ASSERT_EQ(s.GetQueueSize(), PlaybackSession::kMaxQueueSize);
    }

    // 4. Malformed JSON returns error status without crashing
    {
        tempDir.WriteFile(L"session.json", "{ malformed json :::");
        PlaybackSession s;
        StorageStatus st = s.Load(sessionPath, tempDir.GetPath());
        TEST_ASSERT(!st.is_ok());
        TEST_ASSERT_EQ(s.GetState(), PlaybackState::Closed);
    }
}

TEST_CASE(PlaybackSession, RootRelativePortability) {
    ScopedTempDir rootDirA(L"Aperture_RootA_");
    rootDirA.CreateSubDir(L"media");
    rootDirA.CreateSubDir(L"media\\sub");
    std::wstring file1A = rootDirA.SubPath(L"media\\clip1.mp4");
    std::wstring file2A = rootDirA.SubPath(L"media\\sub\\clip2.mp4");
    rootDirA.WriteFile(L"media\\clip1.mp4", "clip 1");
    rootDirA.WriteFile(L"media\\sub\\clip2.mp4", "clip 2");

    std::wstring sessionPathA = rootDirA.SubPath(L"session.json");

    PlaybackSession sessionA;
    sessionA.OpenQueue({ file1A, file2A }, 1);
    sessionA.SetPlaybackPosition(33.0);
    TEST_ASSERT(sessionA.Save(sessionPathA, rootDirA.GetPath()).is_ok());

    // Verify paths are stored relative in JSON
    std::string jsonContent;
    TEST_ASSERT(ReadTextFile(sessionPathA, jsonContent).is_ok());
    JsonValue root;
    TEST_ASSERT(JsonValue::parse(jsonContent, root));
    TEST_ASSERT_EQ(root.get("currentFilePath").as_string(), ".\\media\\sub\\clip2.mp4");
    const auto& arr = root.get("queue").as_array();
    TEST_ASSERT_EQ(arr.size(), 2ull);
    TEST_ASSERT_EQ(arr[0].as_string(), ".\\media\\clip1.mp4");
    TEST_ASSERT_EQ(arr[1].as_string(), ".\\media\\sub\\clip2.mp4");

    // Simulate relocation to RootB
    ScopedTempDir rootDirB(L"Aperture_RootB_");
    rootDirB.CreateSubDir(L"media");
    rootDirB.CreateSubDir(L"media\\sub");
    std::wstring file1B = rootDirB.SubPath(L"media\\clip1.mp4");
    std::wstring file2B = rootDirB.SubPath(L"media\\sub\\clip2.mp4");
    rootDirB.WriteFile(L"media\\clip1.mp4", "clip 1");
    rootDirB.WriteFile(L"media\\sub\\clip2.mp4", "clip 2");

    std::wstring sessionPathB = rootDirB.SubPath(L"session.json");
    rootDirB.WriteFile(L"session.json", jsonContent);

    PlaybackSession sessionB;
    TEST_ASSERT(sessionB.Load(sessionPathB, rootDirB.GetPath()).is_ok());
    TEST_ASSERT_EQ(sessionB.GetQueueSize(), 2ull);
    TEST_ASSERT_EQ(sessionB.GetQueue()[0], file1B);
    TEST_ASSERT_EQ(sessionB.GetQueue()[1], file2B);
    TEST_ASSERT_EQ(sessionB.GetCurrentFilePath(), file2B);
    TEST_ASSERT_EQ(sessionB.GetQueueIndex(), 1ull);
    TEST_ASSERT_EQ(sessionB.GetPlaybackPosition(), 33.0);
}

TEST_CASE(PlaybackSession, SessionSchemaMigrationAndUnknownFields) {
    ScopedTempDir tempDir(L"Aperture_Session_Migrate_");
    std::wstring mediaFile = tempDir.SubPath(L"video.mp4");
    tempDir.WriteFile(L"video.mp4", "data");
    std::wstring sessionPath = tempDir.SubPath(L"session.json");

    // Unversioned (v0) JSON with extra unknown fields
    std::string v0Json = "{\n"
        "  \"queueMode\": \"direct\",\n"
        "  \"currentFilePath\": \".\\\\video.mp4\",\n"
        "  \"playbackPositionSec\": 50.0,\n"
        "  \"customSessionMetadata\": {\n"
        "    \"chapter\": 3,\n"
        "    \"notes\": \"resumed from external\"\n"
        "  }\n"
        "}";
    tempDir.WriteFile(L"session.json", v0Json);

    PlaybackSession session;
    TEST_ASSERT(session.Load(sessionPath, tempDir.GetPath()).is_ok());
    TEST_ASSERT_EQ(session.GetLoadedSchemaVersion(), 0);
    TEST_ASSERT_EQ(session.GetCurrentFilePath(), mediaFile);
    TEST_ASSERT_EQ(session.GetPlaybackPosition(), 50.0);

    // Save and verify unknown fields survived and schemaVersion is 1
    TEST_ASSERT(session.Save(sessionPath, tempDir.GetPath()).is_ok());
    std::string savedText;
    TEST_ASSERT(ReadTextFile(sessionPath, savedText).is_ok());
    JsonValue savedRoot;
    TEST_ASSERT(JsonValue::parse(savedText, savedRoot));
    TEST_ASSERT_EQ(savedRoot.get("schemaVersion").as_int(), PlaybackSession::kCurrentSessionSchemaVersion);
    TEST_ASSERT(savedRoot.has_key("customSessionMetadata"));
    TEST_ASSERT_EQ(savedRoot.get("customSessionMetadata").get("chapter").as_int(), 3);
    TEST_ASSERT_EQ(savedRoot.get("customSessionMetadata").get("notes").as_string(), "resumed from external");
}

TEST_CASE(PlaybackSession, SessionBackupRecovery) {
    ScopedTempDir tempDir(L"Aperture_Session_Backup_");
    std::wstring mediaFile = tempDir.SubPath(L"video.mp4");
    tempDir.WriteFile(L"video.mp4", "data");
    std::wstring sessionPath = tempDir.SubPath(L"session.json");

    PlaybackSession session;
    session.OpenDirect(mediaFile, 77.0);
    TEST_ASSERT(session.Save(sessionPath, tempDir.GetPath()).is_ok());

    // Update position and save again to create rotating backup
    session.SetPlaybackPosition(88.0);
    TEST_ASSERT(session.Save(sessionPath, tempDir.GetPath()).is_ok());

    // Corrupt the main session.json file
    tempDir.WriteFile(L"session.json", "{{ corrupted garbage !!");

    PlaybackSession recoveredSession;
    TEST_ASSERT(recoveredSession.Load(sessionPath, tempDir.GetPath()).is_ok());
    TEST_ASSERT(recoveredSession.WasRecoveredFromBackup());
    TEST_ASSERT_EQ(recoveredSession.GetCurrentFilePath(), mediaFile);
    TEST_ASSERT(recoveredSession.GetPlaybackPosition() >= 77.0);
}

TEST_CASE(PlaybackSession, ActivePreferencesTracking) {
    PlaybackSession session;
    TEST_ASSERT(!session.GetActivePreferences().has_value());

    MediaPlaybackPreferences prefs;
    prefs.playbackSpeed = 1.25;
    prefs.aspectRatio = "16:9";
    prefs.audioDelayMs = 100;
    prefs.subDelayMs = -200;
    prefs.subtitlesVisible = true;

    session.SetActivePreferences(prefs);
    TEST_ASSERT(session.GetActivePreferences().has_value());
    TEST_ASSERT_EQ(*session.GetActivePreferences()->playbackSpeed, 1.25);
    TEST_ASSERT_EQ(*session.GetActivePreferences()->aspectRatio, std::string("16:9"));
    TEST_ASSERT_EQ(*session.GetActivePreferences()->audioDelayMs, 100);
    TEST_ASSERT_EQ(*session.GetActivePreferences()->subDelayMs, -200);
    TEST_ASSERT_EQ(*session.GetActivePreferences()->subtitlesVisible, true);

    // OpenDirect clears active preferences
    session.OpenDirect(L"C:\\media\\video2.mp4");
    TEST_ASSERT(!session.GetActivePreferences().has_value());

    // Set and clear
    session.SetActivePreferences(prefs);
    TEST_ASSERT(session.GetActivePreferences().has_value());
    session.ClearActivePreferences();
    TEST_ASSERT(!session.GetActivePreferences().has_value());
}
TEST_CASE(PlaybackSession, ActivePreferencesOwnCopyOfLibraryRecord) {
    MediaItem item;
    item.preferences.emplace();
    item.preferences->playbackSpeed = 1.5;
    item.preferences->extraFields["plugin"]["setting"] = "original";
    PlaybackSession session;
    session.SetActivePreferences(item.preferences);
    item.preferences->playbackSpeed = 2.0;
    item.preferences->extraFields["plugin"]["setting"] = "changed";
    TEST_ASSERT_EQ(*session.GetActivePreferences()->playbackSpeed, 1.5);
    TEST_ASSERT_EQ(session.GetActivePreferences()->extraFields.get("plugin").get("setting").as_string(), std::string("original"));
    session.SetActivePreferences(item.preferences);
    TEST_ASSERT_EQ(*session.GetActivePreferences()->playbackSpeed, 2.0);
    item.preferences.reset();
    session.SetActivePreferences(item.preferences);
    TEST_ASSERT(!session.GetActivePreferences());
    session.SetActivePreferences(MediaPlaybackPreferences{});
    TEST_ASSERT(session.GetActivePreferences().has_value());
    session.SetActivePreferences(std::nullopt);
    TEST_ASSERT(!session.GetActivePreferences());
}

TEST_CASE(PlaybackSession, PiPTransitionsAndGeometry) {
    PlaybackSession session;
    HWND testWnd = CreateWindowExW(
        WS_EX_OVERLAPPEDWINDOW, L"STATIC", L"PiPTest",
        WS_OVERLAPPEDWINDOW, 100, 100, 800, 600,
        NULL, NULL, GetModuleHandleW(NULL), NULL
    );
    TEST_ASSERT(testWnd != NULL);

    // 1. Initial windowed state
    TEST_ASSERT(session.IsWindowed());
    TEST_ASSERT(!session.IsFullscreen());
    TEST_ASSERT(!session.IsPiP());
    TEST_ASSERT_EQ(session.GetPriorPresentationMode(), PresentationMode::Windowed);

    // 2. Windowed -> Fullscreen
    TEST_ASSERT(session.EnterFullscreen(testWnd));
    TEST_ASSERT(session.IsFullscreen());
    TEST_ASSERT(!session.IsPiP());

    // 3. Fullscreen -> PiP (must remember prior mode as Fullscreen)
    PiPGeometry savedGeom{ 200, 200, 500, 300 };
    TEST_ASSERT(session.EnterPiP(testWnd, savedGeom));
    TEST_ASSERT(session.IsPiP());
    TEST_ASSERT(!session.IsFullscreen());
    TEST_ASSERT_EQ(session.GetPriorPresentationMode(), PresentationMode::Fullscreen);

    // 4. PiP -> Exit PiP (must return to Fullscreen)
    TEST_ASSERT(session.ExitPiP(testWnd));
    TEST_ASSERT(session.IsFullscreen());
    TEST_ASSERT(!session.IsPiP());
    TEST_ASSERT_EQ(session.GetPriorPresentationMode(), PresentationMode::Windowed);
    RECT fullscreenRect{};
    TEST_ASSERT(GetWindowRect(testWnd, &fullscreenRect));
    HMONITOR transitionMonitor = MonitorFromWindow(testWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO transitionMonitorInfo = { sizeof(MONITORINFO) };
    TEST_ASSERT(GetMonitorInfo(transitionMonitor, &transitionMonitorInfo));
    TEST_ASSERT(EqualRect(&fullscreenRect, &transitionMonitorInfo.rcMonitor));

    // 5. Exit Fullscreen -> returns to original Windowed placement
    TEST_ASSERT(session.ExitFullscreen(testWnd));
    TEST_ASSERT(session.IsWindowed());
    TEST_ASSERT(!session.IsFullscreen());

    // 6. Windowed -> PiP with off-screen / invalid geometry (clamping to active work area)
    PiPGeometry offScreenGeom{ -99999, -99999, 50000, 50000 };
    TEST_ASSERT(session.EnterPiP(testWnd, offScreenGeom));
    TEST_ASSERT(session.IsPiP());
    TEST_ASSERT_EQ(session.GetPriorPresentationMode(), PresentationMode::Windowed);

    RECT pipRect{};
    GetWindowRect(testWnd, &pipRect);
    HMONITOR hMon = MonitorFromWindow(testWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    GetMonitorInfo(hMon, &mi);
    TEST_ASSERT(pipRect.left >= mi.rcWork.left);
    TEST_ASSERT(pipRect.top >= mi.rcWork.top);
    TEST_ASSERT((pipRect.right - pipRect.left) <= (mi.rcWork.right - mi.rcWork.left));
    TEST_ASSERT((pipRect.bottom - pipRect.top) <= (mi.rcWork.bottom - mi.rcWork.top));

    // 7. Exit PiP back to Windowed
    TEST_ASSERT(session.ExitPiP(testWnd));
    TEST_ASSERT(session.IsWindowed());

    DestroyWindow(testWnd);
}
TEST_CASE(PlaybackSession, MaximizedWindowedPiPRoundTrip) {
    PlaybackSession session;
    HWND testWnd = CreateWindowExW(
        WS_EX_OVERLAPPEDWINDOW, L"STATIC", L"PiPMaximizedTest",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 800, 600,
        NULL, NULL, GetModuleHandleW(NULL), NULL
    );
    TEST_ASSERT(testWnd != NULL);

    ShowWindow(testWnd, SW_SHOWNORMAL);
    UpdateWindow(testWnd);

    ShowWindow(testWnd, SW_MAXIMIZE);
    UpdateWindow(testWnd);
    TEST_ASSERT(IsZoomed(testWnd));

    WINDOWPLACEMENT wpMax = { sizeof(WINDOWPLACEMENT) };
    GetWindowPlacement(testWnd, &wpMax);
    TEST_ASSERT_EQ(wpMax.showCmd, SW_SHOWMAXIMIZED);

    HMONITOR hMon = MonitorFromWindow(testWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    GetMonitorInfo(hMon, &mi);

    RECT maxRectBefore{};
    GetWindowRect(testWnd, &maxRectBefore);
    TEST_ASSERT(maxRectBefore.left <= mi.rcWork.left);
    TEST_ASSERT(maxRectBefore.top <= mi.rcWork.top);
    TEST_ASSERT(maxRectBefore.right >= mi.rcWork.right);
    TEST_ASSERT(maxRectBefore.bottom >= mi.rcWork.bottom);

    PiPGeometry pipGeom{ mi.rcWork.left + 200, mi.rcWork.top + 200, 480, 270 };
    TEST_ASSERT(session.EnterPiP(testWnd, pipGeom));
    TEST_ASSERT(session.IsPiP());
    TEST_ASSERT(!IsZoomed(testWnd));

    RECT pipRect{};
    GetWindowRect(testWnd, &pipRect);
    int pipW = pipRect.right - pipRect.left;
    int pipH = pipRect.bottom - pipRect.top;
    TEST_ASSERT(pipW <= 500 && pipH <= 300);

    // Exit PiP back to maximized
    TEST_ASSERT(session.ExitPiP(testWnd));
    TEST_ASSERT(session.IsWindowed());
    TEST_ASSERT(!session.IsPiP());

    WINDOWPLACEMENT wpAfter = { sizeof(WINDOWPLACEMENT) };
    GetWindowPlacement(testWnd, &wpAfter);
    bool zoomedAfter = IsZoomed(testWnd);
    RECT boundsAfter{};
    GetWindowRect(testWnd, &boundsAfter);

    // Assert maximized state restored immediately and covers rcWork
    TEST_ASSERT(zoomedAfter);
    TEST_ASSERT_EQ(wpAfter.showCmd, SW_SHOWMAXIMIZED);
    TEST_ASSERT(boundsAfter.left <= mi.rcWork.left);
    TEST_ASSERT(boundsAfter.top <= mi.rcWork.top);
    TEST_ASSERT(boundsAfter.right >= mi.rcWork.right);
    TEST_ASSERT(boundsAfter.bottom >= mi.rcWork.bottom);

    // Verify dragging/restoring: restoring the maximized window must restore to rcNormalPosition, NOT to PiP size
    ShowWindow(testWnd, SW_RESTORE);
    UpdateWindow(testWnd);
    TEST_ASSERT(!IsZoomed(testWnd));
    RECT restoredRect{};
    GetWindowRect(testWnd, &restoredRect);
    int restoredW = restoredRect.right - restoredRect.left;
    int restoredH = restoredRect.bottom - restoredRect.top;
    int normalW = wpAfter.rcNormalPosition.right - wpAfter.rcNormalPosition.left;
    int normalH = wpAfter.rcNormalPosition.bottom - wpAfter.rcNormalPosition.top;
    TEST_ASSERT(std::abs(restoredW - normalW) <= 20);
    TEST_ASSERT(std::abs(restoredH - normalH) <= 20);
    TEST_ASSERT(restoredW >= 700 && restoredH >= 500); // Definitely NOT the 480x270 PiP size

    DestroyWindow(testWnd);
}

TEST_CASE(PlaybackSession, NormalWindowedPiPRoundTrip) {
    PlaybackSession session;
    HWND testWnd = CreateWindowExW(
        WS_EX_OVERLAPPEDWINDOW, L"STATIC", L"PiPNormalTest",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 120, 140, 800, 600,
        NULL, NULL, GetModuleHandleW(NULL), NULL
    );
    TEST_ASSERT(testWnd != NULL);
    ShowWindow(testWnd, SW_SHOWNORMAL);
    UpdateWindow(testWnd);

    RECT normalBefore{};
    GetWindowRect(testWnd, &normalBefore);
    int origW = normalBefore.right - normalBefore.left;
    int origH = normalBefore.bottom - normalBefore.top;

    PiPGeometry pipGeom{ 300, 300, 480, 270 };
    TEST_ASSERT(session.EnterPiP(testWnd, pipGeom));
    TEST_ASSERT(session.IsPiP());
    TEST_ASSERT(!IsZoomed(testWnd));

    TEST_ASSERT(session.ExitPiP(testWnd));
    TEST_ASSERT(session.IsWindowed());
    TEST_ASSERT(!session.IsPiP());
    TEST_ASSERT(!IsZoomed(testWnd));

    RECT normalAfter{};
    GetWindowRect(testWnd, &normalAfter);
    int afterW = normalAfter.right - normalAfter.left;
    int afterH = normalAfter.bottom - normalAfter.top;

    TEST_ASSERT(std::abs(normalAfter.left - normalBefore.left) <= 10);
    TEST_ASSERT(std::abs(normalAfter.top - normalBefore.top) <= 10);
    TEST_ASSERT(std::abs(afterW - origW) <= 10);
    TEST_ASSERT(std::abs(afterH - origH) <= 10);

    DestroyWindow(testWnd);
}

TEST_CASE(PlaybackSession, FullscreenPiPRoundTrip) {
    PlaybackSession session;
    HWND testWnd = CreateWindowExW(
        WS_EX_OVERLAPPEDWINDOW, L"STATIC", L"PiPFullscreenTest",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 800, 600,
        NULL, NULL, GetModuleHandleW(NULL), NULL
    );
    TEST_ASSERT(testWnd != NULL);

    // Path 1: Maximized Windowed -> Fullscreen -> PiP -> Exit PiP -> Exit Fullscreen
    ShowWindow(testWnd, SW_MAXIMIZE);
    UpdateWindow(testWnd);
    TEST_ASSERT(IsZoomed(testWnd));

    TEST_ASSERT(session.EnterFullscreen(testWnd));
    TEST_ASSERT(session.IsFullscreen());
    TEST_ASSERT(!session.IsPiP());

    PiPGeometry pipGeom{ 250, 250, 480, 270 };
    TEST_ASSERT(session.EnterPiP(testWnd, pipGeom));
    TEST_ASSERT(session.IsPiP());
    TEST_ASSERT(!session.IsFullscreen());
    TEST_ASSERT_EQ(session.GetPriorPresentationMode(), PresentationMode::Fullscreen);

    // Exit PiP must restore Fullscreen
    TEST_ASSERT(session.ExitPiP(testWnd));
    TEST_ASSERT(session.IsFullscreen());
    TEST_ASSERT(!session.IsPiP());

    // Exit Fullscreen must restore the original Maximized window
    TEST_ASSERT(session.ExitFullscreen(testWnd));
    TEST_ASSERT(session.IsWindowed());
    TEST_ASSERT(!session.IsFullscreen());
    TEST_ASSERT(IsZoomed(testWnd));

    WINDOWPLACEMENT wp{};
    wp.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(testWnd, &wp);
    TEST_ASSERT_EQ(wp.showCmd, SW_SHOWMAXIMIZED);

    // Path 2: Normal Windowed -> Fullscreen -> PiP -> Exit PiP -> Exit Fullscreen
    ShowWindow(testWnd, SW_RESTORE);
    SetWindowPos(testWnd, NULL, 150, 150, 750, 550, SWP_NOZORDER | SWP_FRAMECHANGED);
    UpdateWindow(testWnd);
    TEST_ASSERT(!IsZoomed(testWnd));

    TEST_ASSERT(session.EnterFullscreen(testWnd));
    TEST_ASSERT(session.IsFullscreen());

    TEST_ASSERT(session.EnterPiP(testWnd, pipGeom));
    TEST_ASSERT(session.IsPiP());

    TEST_ASSERT(session.ExitPiP(testWnd));
    TEST_ASSERT(session.IsFullscreen());

    TEST_ASSERT(session.ExitFullscreen(testWnd));
    TEST_ASSERT(session.IsWindowed());
    TEST_ASSERT(!IsZoomed(testWnd));

    RECT rectAfterNormalFs{};
    GetWindowRect(testWnd, &rectAfterNormalFs);
    int finalW = rectAfterNormalFs.right - rectAfterNormalFs.left;
    int finalH = rectAfterNormalFs.bottom - rectAfterNormalFs.top;
    TEST_ASSERT(std::abs(finalW - 750) <= 20);
    TEST_ASSERT(std::abs(finalH - 550) <= 20);

    DestroyWindow(testWnd);
}

TEST_CASE(PlaybackSession, FullscreenExitRestoresPlacementWithoutIntermediateSize) {
    struct WindowProbe {
        HWND window = CreateWindowExW(0, L"STATIC", L"Fullscreen restore test",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 140, 120, 800, 600,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        std::vector<RECT> sizes;
        static LRESULT CALLBACK Observe(HWND hwnd, UINT message, WPARAM wParam,
            LPARAM lParam, UINT_PTR, DWORD_PTR context) {
            auto& probe = *reinterpret_cast<WindowProbe*>(context);
            if (message == WM_SIZE) {
                RECT bounds{};
                if (GetWindowRect(hwnd, &bounds)) probe.sizes.push_back(bounds);
            }
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }
        ~WindowProbe() { if (window) DestroyWindow(window); }
    } probe;
    TEST_ASSERT(probe.window != nullptr);
    if (!probe.window) return;
    TEST_ASSERT(SetWindowSubclass(probe.window, WindowProbe::Observe, 1,
        reinterpret_cast<DWORD_PTR>(&probe)));

    for (bool maximized : {false, true}) {
        for (bool returnToLibrary : {false, true}) {
            PlaybackSession session;
            ShowWindow(probe.window, maximized ? SW_MAXIMIZE : SW_RESTORE);
            session.SetReduceMotion(returnToLibrary);
            RECT expected{};
            TEST_ASSERT(GetWindowRect(probe.window, &expected));
            WINDOWPLACEMENT before{sizeof(WINDOWPLACEMENT)};
            TEST_ASSERT(GetWindowPlacement(probe.window, &before));
            TEST_ASSERT(session.EnterFullscreen(probe.window));
            probe.sizes.clear();
            if (returnToLibrary) session.ResetPresentation(probe.window);
            else TEST_ASSERT(session.ExitFullscreen(probe.window));
            TEST_ASSERT(session.IsWindowed());
            TEST_ASSERT_EQ(IsZoomed(probe.window) != FALSE, maximized);
            RECT actual{};
            TEST_ASSERT(GetWindowRect(probe.window, &actual));
            TEST_ASSERT(EqualRect(&actual, &expected));
            TEST_ASSERT(!probe.sizes.empty());
            for (const RECT& size : probe.sizes) {
                TEST_ASSERT_MSG(EqualRect(&size, &expected),
                    "Fullscreen exit exposed an intermediate window size");
            }
            WINDOWPLACEMENT after{sizeof(WINDOWPLACEMENT)};
            TEST_ASSERT(GetWindowPlacement(probe.window, &after));
            TEST_ASSERT(EqualRect(&before.rcNormalPosition, &after.rcNormalPosition));
        }
    }
}

TEST_CASE(PlaybackSession, PiPFullscreenGeometryPreservation) {
    PlaybackSession session;
    HWND testWnd = CreateWindowExW(
        WS_EX_OVERLAPPEDWINDOW, L"STATIC", L"PiPGeomPreserveTest",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 800, 600,
        NULL, NULL, GetModuleHandleW(NULL), NULL
    );
    TEST_ASSERT(testWnd != NULL);
    ShowWindow(testWnd, SW_SHOWNORMAL);
    UpdateWindow(testWnd);

    HMONITOR hMon = MonitorFromWindow(testWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    GetMonitorInfo(hMon, &mi);

    // Enter PiP with explicit custom geometry
    PiPGeometry pipGeom{ mi.rcWork.left + 220, mi.rcWork.top + 180, 480, 270 };
    TEST_ASSERT(session.EnterPiP(testWnd, pipGeom));
    TEST_ASSERT(session.IsPiP());

    RECT pipRectBeforeFs{};
    GetWindowRect(testWnd, &pipRectBeforeFs);
    int wBeforeFs = pipRectBeforeFs.right - pipRectBeforeFs.left;
    int hBeforeFs = pipRectBeforeFs.bottom - pipRectBeforeFs.top;

    // Transition from PiP directly to Fullscreen
    TEST_ASSERT(session.EnterFullscreen(testWnd));
    TEST_ASSERT(session.IsFullscreen());
    TEST_ASSERT(!session.IsPiP());

    // Verify session captured the PiP geometry
    const auto& capturedGeom = session.GetLastPiPGeometry();
    TEST_ASSERT(capturedGeom.width > 0 && capturedGeom.height > 0);
    TEST_ASSERT(std::abs(capturedGeom.width - wBeforeFs) <= 10);
    TEST_ASSERT(std::abs(capturedGeom.height - hBeforeFs) <= 10);

    // Now re-enter PiP using default/empty geometry: must restore captured PiP geometry
    TEST_ASSERT(session.EnterPiP(testWnd));
    TEST_ASSERT(session.IsPiP());
    RECT pipRectAfterFs{};
    GetWindowRect(testWnd, &pipRectAfterFs);
    int wAfterFs = pipRectAfterFs.right - pipRectAfterFs.left;
    int hAfterFs = pipRectAfterFs.bottom - pipRectAfterFs.top;

    TEST_ASSERT(std::abs(pipRectAfterFs.left - pipRectBeforeFs.left) <= 10);
    TEST_ASSERT(std::abs(pipRectAfterFs.top - pipRectBeforeFs.top) <= 10);
    TEST_ASSERT(std::abs(wAfterFs - wBeforeFs) <= 10);
    TEST_ASSERT(std::abs(hAfterFs - hBeforeFs) <= 10);

    TEST_ASSERT(session.ExitPiP(testWnd));
    TEST_ASSERT(session.IsFullscreen());
    TEST_ASSERT(session.ExitFullscreen(testWnd));
    TEST_ASSERT(session.IsWindowed());
    DestroyWindow(testWnd);
}

TEST_CASE(PlaybackSession, PiPOverlayFollowsClientOriginAfterDrag) {
    HWND owner = CreateWindowExW(
        0, L"STATIC", L"PiPOverlayOwnerTest",
        WS_POPUP | WS_THICKFRAME | WS_VISIBLE, 160, 140, 520, 320,
        NULL, NULL, GetModuleHandleW(NULL), NULL
    );
    TEST_ASSERT(owner != NULL);

    HWND overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"STATIC", L"",
        WS_POPUP | WS_VISIBLE, 0, 0, 1, 1,
        owner, NULL, GetModuleHandleW(NULL), NULL
    );
    TEST_ASSERT(overlay != NULL);

    ShowWindow(owner, SW_SHOWNORMAL);
    UpdateWindow(owner);

    const POINT dragPositions[] = {
        { 160, 140 },
        { 284, 226 },
        { 418, 312 }
    };
    for (const POINT& position : dragPositions) {
        SetWindowPos(owner, NULL, position.x, position.y, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

        RECT outerRect{};
        TEST_ASSERT(GetWindowRect(owner, &outerRect));
        RECT expectedClientRect{};
        TEST_ASSERT(PlaceWindowOverClient(owner, overlay, HWND_TOP,
            SWP_NOACTIVATE | SWP_SHOWWINDOW, &expectedClientRect));
        TEST_ASSERT(expectedClientRect.left != outerRect.left || expectedClientRect.top != outerRect.top ||
            expectedClientRect.right != outerRect.right || expectedClientRect.bottom != outerRect.bottom);

        RECT actualOverlayRect{};
        TEST_ASSERT(GetWindowRect(overlay, &actualOverlayRect));
        TEST_ASSERT(EqualRect(&actualOverlayRect, &expectedClientRect));
    }

    DestroyWindow(overlay);
    DestroyWindow(owner);
}

TEST_CASE(PlaybackSession, SettingsPiPPersistence) {
    Settings settings;
    // Default sentinels
    TEST_ASSERT_EQ(settings.pipX, -1);
    TEST_ASSERT_EQ(settings.pipY, -1);
    TEST_ASSERT_EQ(settings.pipWidth, 0);
    TEST_ASSERT_EQ(settings.pipHeight, 0);

    // Reset retains defaults
    settings.pipX = 150;
    settings.pipY = 250;
    settings.pipWidth = 640;
    settings.pipHeight = 360;
    settings.ResetToDefaults();
    TEST_ASSERT_EQ(settings.pipX, -1);
    TEST_ASSERT_EQ(settings.pipY, -1);
    TEST_ASSERT_EQ(settings.pipWidth, 0);
    TEST_ASSERT_EQ(settings.pipHeight, 0);
}

TEST_CASE(PlaybackSession, PiPToFullscreenTransitionUsesAtomicTargetBounds) {
    PlaybackSession session;
    HWND testWnd = CreateWindowExW(
        WS_EX_OVERLAPPEDWINDOW, L"STATIC", L"PiPTransitionTest",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 800, 600,
        NULL, NULL, GetModuleHandleW(NULL), NULL
    );
    TEST_ASSERT(testWnd != NULL);
    ShowWindow(testWnd, SW_SHOWNORMAL);
    UpdateWindow(testWnd);

    HMONITOR hMon = MonitorFromWindow(testWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    TEST_ASSERT(GetMonitorInfo(hMon, &mi));
    PiPGeometry pipGeom{ mi.rcWork.right - 480, mi.rcWork.bottom - 270, 480, 270 };

    TEST_ASSERT(session.EnterPiP(testWnd, pipGeom));
    session.SetReduceMotion(true);
    TEST_ASSERT(session.EnterFullscreen(testWnd));
    RECT reducedMotionRect{};
    TEST_ASSERT(GetWindowRect(testWnd, &reducedMotionRect));
    TEST_ASSERT(EqualRect(&reducedMotionRect, &mi.rcMonitor));

    TEST_ASSERT(session.EnterPiP(testWnd, pipGeom));
    session.SetReduceMotion(false);
    TEST_ASSERT(session.ExitPiP(testWnd));
    RECT animatedRect{};
    TEST_ASSERT(GetWindowRect(testWnd, &animatedRect));
    TEST_ASSERT(EqualRect(&animatedRect, &mi.rcMonitor));
    TEST_ASSERT(session.IsFullscreen());
    TEST_ASSERT(!session.IsPiP());

    DestroyWindow(testWnd);
}
} // namespace Aperture::Test
