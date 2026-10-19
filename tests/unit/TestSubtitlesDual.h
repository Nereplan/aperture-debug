#pragma once

#include "../support/TestFramework.h"
#include "../../src/Subtitles.h"
#include "../../src/SubtitleDiscovery.h"
#include <limits>
#include <set>

namespace Aperture::Test {

// Independent linear oracle: deliberately ignores the render cache and index.
inline std::wstring SubtitleLookupReference(const SubtitleTrack& track, double time, int delayMs = 0) {
    time -= static_cast<double>(SubtitleDiscovery::ClampDelayMs(delayMs)) / 1000.0;
    if (!std::isfinite(time) || time < 0) return L"";
    std::wstring text;
    std::set<std::wstring> seen;
    size_t count = 0;
    for (const auto& cue : track.cues) {
        if (cue.startTimeSec > time) break;
        if (cue.endTimeSec < time || cue.text.empty() || !seen.insert(cue.text).second) continue;
        if (!text.empty()) text += L'\n';
        text += cue.text;
        if (++count == 64 || text.size() >= SubtitleEngine::kMaxCueTextBytes) break;
    }
    return text;
}
static void step_SubtitlesDual_IndexedOverlapsMatchLinearLookupAcrossBoundariesAndSeeks() {
    SubtitleEngine engine;
    SubtitleTrack track;
    track.cues.push_back({0, 2000, L"long running"});
    for (int i = 0; i < 1025; ++i) {
        const double start = i / 3;
        const double duration = (i * 37) % 19;
        track.cues.push_back({start, start + duration, i % 11 == 0 ? L"" : L"cue " + std::to_wstring(i % 73)});
    }
    engine.tracks.push_back(std::move(track));
    engine.SelectPrimaryTrack(0);
    engine.SelectSecondaryTrack(0);
    engine.SetSecondaryDelayMs(-500);
    const double infinity = std::numeric_limits<double>::infinity();
    auto check = [&](double time) {
        TEST_ASSERT_EQ(engine.GetPrimarySubtitleText(time), SubtitleLookupReference(engine.tracks[0], time));
        TEST_ASSERT_EQ(engine.GetSecondarySubtitleText(time), SubtitleLookupReference(engine.tracks[0], time, -500));
    };
    for (int i = 0; i < 360; ++i) {
        check(i);
        check(std::nextafter(static_cast<double>(i), infinity));
        check(i + 0.5);
    }
    uint32_t seed = 20261005;
    for (int i = 0; i < 500; ++i) {
        seed = seed * 1664525u + 1013904223u;
        check(static_cast<double>(seed % 1440) / 4.0);
    }
    check(2000);
    check(std::nextafter(2000.0, infinity));
    check(-1);
    check(infinity);
    check(std::numeric_limits<double>::quiet_NaN());
}
static void step_SubtitlesDual_IndexedLookupPreservesOutputLimitsAndDuplicateExpiration() {
    SubtitleEngine engine;
    SubtitleTrack track;
    for (int i = 0; i < 200; ++i) track.cues.push_back({0, 1.0 + i, L"cue " + std::to_wstring(i / 2)});
    engine.tracks.push_back(std::move(track));
    engine.SelectPrimaryTrack(0);
    for (double time : {0.0, 1.0, 1.5, 2.0, 2.5, 100.0, 200.0, 201.0, 0.5}) {
        TEST_ASSERT_EQ(engine.GetPrimarySubtitleText(time), SubtitleLookupReference(engine.tracks[0], time));
    }
    engine.tracks[0].cues[0].text.assign(SubtitleEngine::kMaxCueTextBytes, L'x');
    ++engine.tracksGeneration;
    TEST_ASSERT_EQ(engine.GetPrimarySubtitleText(0), SubtitleLookupReference(engine.tracks[0], 0));
}
static void step_SubtitlesDual_IndexedLookupRefreshesForTrackChangesAndClear() {
    SubtitleEngine engine;
    for (int trackIndex = 0; trackIndex < 2; ++trackIndex) {
        SubtitleTrack track;
        for (int i = 0; i < 129; ++i) track.cues.push_back({double(i), i + 0.5, std::to_wstring(trackIndex) + L" cue"});
        engine.tracks.push_back(std::move(track));
    }
    for (int index : {0, 1, 0}) {
        engine.SelectPrimaryTrack(index);
        TEST_ASSERT_EQ(engine.GetPrimarySubtitleText(128.25), SubtitleLookupReference(engine.tracks[index], 128.25));
    }
    engine.tracks[0].cues[0] = {0, 500, L"edited"};
    ++engine.tracksGeneration;
    TEST_ASSERT_EQ(engine.GetPrimarySubtitleText(128.25), std::wstring(L"edited\n0 cue"));
    engine.tracks[0].cues.resize(1);
    TEST_ASSERT_EQ(engine.GetPrimarySubtitleText(128.25), std::wstring(L"edited"));
    engine.tracks[0].cues.clear();
    TEST_ASSERT(engine.GetPrimarySubtitleText(128.25).empty());
    engine.Clear();
    SubtitleTrack replacement;
    replacement.cues = {{0, 500, L"replacement"}};
    engine.tracks.push_back(std::move(replacement));
    engine.SelectPrimaryTrack(0);
    TEST_ASSERT_EQ(engine.GetPrimarySubtitleText(128.25), std::wstring(L"replacement"));
}
static void step_SubtitlesDual_NestedCuesKeepInclusiveEndsAndCacheBoundariesAcrossSeeks() {
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
static void step_SubtitlesDual_PrimaryAndSecondarySelection() {
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
static void step_SubtitlesDual_SelectionFallbackOnInvalidTrack() {
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
static void step_SubtitlesDual_IndependentVisibilityControl() {
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
static void step_SubtitlesDual_IndependentDelayAndClamping() {
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
static void step_SubtitlesDual_TrackCycling() {
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
static void step_SubtitlesDual_PreferencesExportAndImport() {
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


TEST_CASE(SubtitlesDual, IndexedCueOracleAndTrackProfiles) {
    step_SubtitlesDual_IndexedOverlapsMatchLinearLookupAcrossBoundariesAndSeeks();
    step_SubtitlesDual_IndexedLookupPreservesOutputLimitsAndDuplicateExpiration();
    step_SubtitlesDual_IndexedLookupRefreshesForTrackChangesAndClear();
    step_SubtitlesDual_NestedCuesKeepInclusiveEndsAndCacheBoundariesAcrossSeeks();
    step_SubtitlesDual_PrimaryAndSecondarySelection();
    step_SubtitlesDual_SelectionFallbackOnInvalidTrack();
    step_SubtitlesDual_IndependentVisibilityControl();
    step_SubtitlesDual_IndependentDelayAndClamping();
    step_SubtitlesDual_TrackCycling();
    step_SubtitlesDual_PreferencesExportAndImport();
}

} // namespace Aperture::Test
