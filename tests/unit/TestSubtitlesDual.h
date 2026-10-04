#pragma once

#include "../support/TestFramework.h"
#include "../../src/Subtitles.h"
#include "../../src/SubtitleDiscovery.h"
#include <limits>

namespace Aperture::Test {

TEST_CASE(SubtitlesDual, NestedCuesKeepInclusiveEndsAndCacheBoundariesAcrossSeeks) {
    SubtitleEngine engine;
    SubtitleTrack track;
    track.cues = {{0, 100, L"long"}, {1, 2, L"short"}, {2, 5, L"nested"}, {2, 20, L"nested"},
                  {3, 3, L"point"}, {4, 10, L""}, {5, 6, L"last"}};
    engine.tracks.push_back(std::move(track));
    engine.SelectPrimaryTrack(0);
    const double infinity = std::numeric_limits<double>::infinity();
    TEST_ASSERT_EQ(engine.GetCurrentSubtitleText(1.5), std::wstring(L"long\nshort"));
    TEST_ASSERT_EQ(engine.GetCurrentSubtitleText(2), std::wstring(L"long\nshort\nnested"));
    TEST_ASSERT_EQ(engine.GetCurrentSubtitleText(std::nextafter(2.0, infinity)), std::wstring(L"long\nnested"));
    TEST_ASSERT_EQ(engine.GetCurrentSubtitleText(3), std::wstring(L"long\nnested\npoint"));
    TEST_ASSERT_EQ(engine.GetCurrentSubtitleText(3.25), std::wstring(L"long\nnested"));
    TEST_ASSERT_EQ(engine.GetCurrentSubtitleText(5.5), std::wstring(L"long\nnested\nlast"));
    TEST_ASSERT_EQ(engine.GetCurrentSubtitleText(6), std::wstring(L"long\nnested\nlast"));
    TEST_ASSERT_EQ(engine.GetCurrentSubtitleText(std::nextafter(6.0, infinity)), std::wstring(L"long\nnested"));
    TEST_ASSERT_EQ(engine.GetCurrentSubtitleText(0.5), std::wstring(L"long"));
    TEST_ASSERT_EQ(engine.GetCurrentSubtitleText(100), std::wstring(L"long"));
    TEST_ASSERT(engine.GetCurrentSubtitleText(std::nextafter(100.0, infinity)).empty());
    TEST_ASSERT_EQ(engine.GetCurrentSubtitleText(2, 500), std::wstring(L"long\nshort"));
}

TEST_CASE(SubtitlesDual, InitialState) {
    SubtitleEngine engine;
    TEST_ASSERT_EQ(engine.GetTrackCount(), 0);
    TEST_ASSERT_EQ(engine.GetPrimaryTrackIndex(), -1);
    TEST_ASSERT_EQ(engine.GetSecondaryTrackIndex(), -1);
    TEST_ASSERT_EQ(engine.activeTrackIndex, -1);
    TEST_ASSERT(engine.GetPrimaryTrack() == nullptr);
    TEST_ASSERT(engine.GetSecondaryTrack() == nullptr);
    TEST_ASSERT_EQ(engine.GetPrimaryDelayMs(), 0);
    TEST_ASSERT_EQ(engine.GetSecondaryDelayMs(), 0);
    TEST_ASSERT(engine.IsPrimaryVisible());
    TEST_ASSERT(engine.IsSecondaryVisible());
    TEST_ASSERT_EQ(engine.primaryPosition, std::string("bottom"));
    TEST_ASSERT_EQ(engine.secondaryPosition, std::string("top"));
}

TEST_CASE(SubtitlesDual, PrimaryAndSecondarySelection) {
    SubtitleEngine engine;

    // Create 2 test tracks
    SubtitleTrack track1;
    track1.id = "sub_1";
    track1.name = L"English";
    track1.filePath = L"C:\\media\\movie.en.srt";
    SubtitleCue cue1;
    cue1.startTimeSec = 1.0;
    cue1.endTimeSec = 5.0;
    cue1.text = L"Hello World";
    track1.cues.push_back(cue1);

    SubtitleTrack track2;
    track2.id = "sub_2";
    track2.name = L"Spanish";
    track2.filePath = L"C:\\media\\movie.es.srt";
    SubtitleCue cue2;
    cue2.startTimeSec = 1.0;
    cue2.endTimeSec = 5.0;
    cue2.text = L"Hola Mundo";
    track2.cues.push_back(cue2);

    engine.tracks.push_back(track1);
    engine.tracks.push_back(track2);

    // Select primary track 0
    engine.SelectPrimaryTrack(0);
    TEST_ASSERT_EQ(engine.GetPrimaryTrackIndex(), 0);
    TEST_ASSERT_EQ(engine.activeTrackIndex, 0);
    TEST_ASSERT(engine.GetPrimaryTrack() != nullptr);
    TEST_ASSERT_EQ(engine.GetPrimaryTrack()->name, std::wstring(L"English"));

    // Select secondary track 1
    engine.SelectSecondaryTrack(1);
    TEST_ASSERT_EQ(engine.GetSecondaryTrackIndex(), 1);
    TEST_ASSERT(engine.GetSecondaryTrack() != nullptr);
    TEST_ASSERT_EQ(engine.GetSecondaryTrack()->name, std::wstring(L"Spanish"));

    // Query dual text at 2.0s
    std::wstring primText, secText;
    engine.GetDualSubtitleText(2.0, primText, secText);
    TEST_ASSERT_EQ(primText, std::wstring(L"Hello World"));
    TEST_ASSERT_EQ(secText, std::wstring(L"Hola Mundo"));
}

TEST_CASE(SubtitlesDual, SelectionFallbackOnInvalidTrack) {
    SubtitleEngine engine;

    SubtitleTrack track1;
    track1.id = "sub_1";
    track1.name = L"Track 1";
    track1.cues.push_back({ 0.0, 10.0, L"Text 1" });
    engine.tracks.push_back(track1);

    engine.SelectPrimaryTrack(0);
    engine.SelectSecondaryTrack(0);
    TEST_ASSERT_EQ(engine.GetPrimaryTrackIndex(), 0);
    TEST_ASSERT_EQ(engine.GetSecondaryTrackIndex(), 0);

    // Out-of-bounds selection falls back to -1 / disabled
    engine.SelectSecondaryTrack(5);
    TEST_ASSERT_EQ(engine.GetSecondaryTrackIndex(), -1);
    TEST_ASSERT(engine.GetSecondaryTrack() == nullptr);
    TEST_ASSERT_EQ(engine.GetSecondarySubtitleText(2.0), std::wstring(L""));

    // Negative index (other than valid) resets to -1
    engine.SelectSecondaryTrack(-5);
    TEST_ASSERT_EQ(engine.GetSecondaryTrackIndex(), -1);

    // Primary still intact
    TEST_ASSERT_EQ(engine.GetPrimaryTrackIndex(), 0);
    TEST_ASSERT_EQ(engine.GetPrimarySubtitleText(2.0), std::wstring(L"Text 1"));
}

TEST_CASE(SubtitlesDual, IndependentVisibilityControl) {
    SubtitleEngine engine;

    SubtitleTrack track1;
    track1.id = "sub_1";
    track1.name = L"English";
    track1.cues.push_back({ 1.0, 5.0, L"English Text" });

    SubtitleTrack track2;
    track2.id = "sub_2";
    track2.name = L"Japanese";
    track2.cues.push_back({ 1.0, 5.0, L"Japanese Text" });

    engine.tracks.push_back(track1);
    engine.tracks.push_back(track2);
    engine.SelectPrimaryTrack(0);
    engine.SelectSecondaryTrack(1);

    // Both visible
    TEST_ASSERT_EQ(engine.GetPrimarySubtitleText(2.0), std::wstring(L"English Text"));
    TEST_ASSERT_EQ(engine.GetSecondarySubtitleText(2.0), std::wstring(L"Japanese Text"));

    // Hide primary, keep secondary
    engine.SetPrimaryVisibility(false);
    TEST_ASSERT(!engine.IsPrimaryVisible());
    TEST_ASSERT(engine.IsSecondaryVisible());
    TEST_ASSERT_EQ(engine.GetPrimarySubtitleText(2.0), std::wstring(L""));
    TEST_ASSERT_EQ(engine.GetSecondarySubtitleText(2.0), std::wstring(L"Japanese Text"));

    // Show primary, hide secondary
    engine.SetPrimaryVisibility(true);
    engine.SetSecondaryVisibility(false);
    TEST_ASSERT(engine.IsPrimaryVisible());
    TEST_ASSERT(!engine.IsSecondaryVisible());
    TEST_ASSERT_EQ(engine.GetPrimarySubtitleText(2.0), std::wstring(L"English Text"));
    TEST_ASSERT_EQ(engine.GetSecondarySubtitleText(2.0), std::wstring(L""));

    // Both hidden
    engine.SetPrimaryVisibility(false);
    std::wstring p, s;
    engine.GetDualSubtitleText(2.0, p, s);
    TEST_ASSERT_EQ(p, std::wstring(L""));
    TEST_ASSERT_EQ(s, std::wstring(L""));
}

TEST_CASE(SubtitlesDual, IndependentDelayAndClamping) {
    SubtitleEngine engine;

    SubtitleTrack track1;
    track1.id = "sub_1";
    track1.name = L"Primary";
    track1.cues.push_back({ 5.0, 10.0, L"Cue at 5-10s" });

    SubtitleTrack track2;
    track2.id = "sub_2";
    track2.name = L"Secondary";
    track2.cues.push_back({ 5.0, 10.0, L"Sec Cue at 5-10s" });

    engine.tracks.push_back(track1);
    engine.tracks.push_back(track2);
    engine.SelectPrimaryTrack(0);
    engine.SelectSecondaryTrack(1);

    // Primary delay +2000ms (+2.0s): cue active when media is at 7.0s (7.0 - 2.0 = 5.0s)
    engine.SetPrimaryDelayMs(2000);
    TEST_ASSERT_EQ(engine.GetPrimaryDelayMs(), 2000);
    TEST_ASSERT_EQ(engine.GetPrimarySubtitleText(5.0), std::wstring(L""));
    TEST_ASSERT_EQ(engine.GetPrimarySubtitleText(7.0), std::wstring(L"Cue at 5-10s"));

    // Secondary delay -2000ms (-2.0s): cue active when media is at 3.0s (3.0 - (-2.0) = 5.0s)
    engine.SetSecondaryDelayMs(-2000);
    TEST_ASSERT_EQ(engine.GetSecondaryDelayMs(), -2000);
    TEST_ASSERT_EQ(engine.GetSecondarySubtitleText(3.0), std::wstring(L"Sec Cue at 5-10s"));
    TEST_ASSERT_EQ(engine.GetSecondarySubtitleText(7.0), std::wstring(L"Sec Cue at 5-10s")); // 7 - (-2) = 9s (in range 5-10)

    // Clamping checks
    engine.SetPrimaryDelayMs(1000000); // Exceeds +600,000
    TEST_ASSERT_EQ(engine.GetPrimaryDelayMs(), SubtitleDiscovery::kMaxSubtitleDelayMs);

    engine.SetSecondaryDelayMs(-1000000); // Exceeds -600,000
    TEST_ASSERT_EQ(engine.GetSecondaryDelayMs(), SubtitleDiscovery::kMinSubtitleDelayMs);
}

TEST_CASE(SubtitlesDual, TrackCycling) {
    SubtitleEngine engine;

    engine.tracks.push_back({ "sub_1", L"Track 1", L"", {{ 0.0, 5.0, L"T1" }}, false });
    engine.tracks.push_back({ "sub_2", L"Track 2", L"", {{ 0.0, 5.0, L"T2" }}, false });

    // Initial state: -1
    TEST_ASSERT_EQ(engine.GetPrimaryTrackIndex(), -1);
    TEST_ASSERT_EQ(engine.GetSecondaryTrackIndex(), -1);

    // Cycle primary: 0 -> 1 -> -1 -> 0
    engine.CyclePrimaryTrack();
    TEST_ASSERT_EQ(engine.GetPrimaryTrackIndex(), 0);
    engine.CyclePrimaryTrack();
    TEST_ASSERT_EQ(engine.GetPrimaryTrackIndex(), 1);
    engine.CyclePrimaryTrack();
    TEST_ASSERT_EQ(engine.GetPrimaryTrackIndex(), -1);
    engine.CyclePrimaryTrack();
    TEST_ASSERT_EQ(engine.GetPrimaryTrackIndex(), 0);

    // Cycle secondary: 0 -> 1 -> -1 -> 0
    engine.CycleSecondaryTrack();
    TEST_ASSERT_EQ(engine.GetSecondaryTrackIndex(), 0);
    engine.CycleSecondaryTrack();
    TEST_ASSERT_EQ(engine.GetSecondaryTrackIndex(), 1);
    engine.CycleSecondaryTrack();
    TEST_ASSERT_EQ(engine.GetSecondaryTrackIndex(), -1);
    engine.CycleSecondaryTrack();
    TEST_ASSERT_EQ(engine.GetSecondaryTrackIndex(), 0);
}

TEST_CASE(SubtitlesDual, PreferencesExportAndImport) {
    SubtitleEngine engine;
    engine.tracks.push_back({ "sub_1", L"English", L"C:\\media\\subs\\movie.en.srt", {{ 0.0, 5.0, L"Hello" }}, false });
    engine.tracks.push_back({ "sub_2", L"Spanish", L"C:\\media\\subs\\movie.es.srt", {{ 0.0, 5.0, L"Hola" }}, false });

    engine.SelectPrimaryTrack(0);
    engine.SelectSecondaryTrack(1);
    engine.SetPrimaryDelayMs(500);
    engine.SetSecondaryDelayMs(-300);
    engine.SetPrimaryVisibility(true);
    engine.SetSecondaryVisibility(false);
    engine.primaryPosition = "bottom";
    engine.secondaryPosition = "top";

    std::wstring rootDir = L"C:\\media";
    SubtitlePreferences prefs = engine.ExportPreferences(rootDir);

    TEST_ASSERT_EQ(prefs.primaryTrackIndex, 0);
    TEST_ASSERT_EQ(prefs.secondaryTrackIndex, 1);
    TEST_ASSERT_EQ(prefs.primaryTrackPath, std::wstring(L"subs\\movie.en.srt"));
    TEST_ASSERT_EQ(prefs.secondaryTrackPath, std::wstring(L"subs\\movie.es.srt"));
    TEST_ASSERT_EQ(prefs.primaryDelayMs, 500);
    TEST_ASSERT_EQ(prefs.secondaryDelayMs, -300);
    TEST_ASSERT(prefs.primaryVisible);
    TEST_ASSERT(!prefs.secondaryVisible);

    std::string jsonStr = prefs.SerializeJson();
    TEST_ASSERT(!jsonStr.empty());

    SubtitlePreferences loadedPrefs;
    TEST_ASSERT(loadedPrefs.DeserializeJson(jsonStr));
    TEST_ASSERT_EQ(loadedPrefs.primaryTrackIndex, 0);
    TEST_ASSERT_EQ(loadedPrefs.secondaryTrackIndex, 1);
    TEST_ASSERT_EQ(loadedPrefs.primaryTrackPath, std::wstring(L"subs\\movie.en.srt"));
    TEST_ASSERT_EQ(loadedPrefs.secondaryTrackPath, std::wstring(L"subs\\movie.es.srt"));
    TEST_ASSERT_EQ(loadedPrefs.primaryDelayMs, 500);
    TEST_ASSERT_EQ(loadedPrefs.secondaryDelayMs, -300);

    // Import into fresh engine
    SubtitleEngine freshEngine;
    freshEngine.tracks = engine.tracks;
    freshEngine.ImportPreferences(loadedPrefs, rootDir);

    TEST_ASSERT_EQ(freshEngine.GetPrimaryTrackIndex(), 0);
    TEST_ASSERT_EQ(freshEngine.GetSecondaryTrackIndex(), 1);
    TEST_ASSERT_EQ(freshEngine.GetPrimaryDelayMs(), 500);
    TEST_ASSERT_EQ(freshEngine.GetSecondaryDelayMs(), -300);
    TEST_ASSERT(freshEngine.IsPrimaryVisible());
    TEST_ASSERT(!freshEngine.IsSecondaryVisible());
}

} // namespace Aperture::Test
