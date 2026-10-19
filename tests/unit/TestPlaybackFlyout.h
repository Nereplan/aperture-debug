#pragma once

#include "../support/TestFramework.h"
#include "../../src/PlaybackFlyoutLayout.h"

namespace Aperture::Test {

TEST_CASE(PlaybackFlyout, HeaderGapsAndPaddingStayDistinct) {
    for (float dp : { 0.5f, 1.0f, 1.5f, 2.0f }) {
        const auto bounds = D2D1::RectF(37, 51, 37 + 280 * dp, 51 + 340 * dp);
        const auto hit = [&](const char* menu, int count, float x, float y) {
            return PlaybackFlyoutLayout::HitTest(menu, count, bounds, 37 + x * dp, 51 + y * dp, dp);
        };
        for (const char* menu : { "queue", "aspect", "speed", "audio", "subtitles" }) {
            TEST_ASSERT_EQ(hit(menu, 3, 140, 25), PlaybackFlyoutLayout::Back);
            TEST_ASSERT_EQ(hit(menu, 3, 140, 2), PlaybackFlyoutLayout::None);
            TEST_ASSERT_EQ(hit(menu, 3, 140, 46), PlaybackFlyoutLayout::None);
            TEST_ASSERT_EQ(hit(menu, 3, 140, 330), PlaybackFlyoutLayout::None);
            TEST_ASSERT_EQ(hit(menu, 3, 2, 65), PlaybackFlyoutLayout::None);
            TEST_ASSERT_EQ(hit(menu, 3, 278, 65), PlaybackFlyoutLayout::None);
        }
        TEST_ASSERT_EQ(hit("queue", 3, 140, 65), PlaybackFlyoutLayout::None);
        TEST_ASSERT_EQ(hit("queue", 3, 140, 90), 0);
        TEST_ASSERT_EQ(hit("queue", 3, 140, 113), PlaybackFlyoutLayout::None);
        TEST_ASSERT_EQ(hit("aspect", 6, 140, 85), PlaybackFlyoutLayout::None);
        TEST_ASSERT_EQ(hit("aspect", 6, 140, 95), 1);
        TEST_ASSERT_EQ(hit("main", 8, 140, 25), 0);
        TEST_ASSERT_EQ(hit("main", 8, 140, 49), PlaybackFlyoutLayout::None);
    }
}

TEST_CASE(PlaybackFlyout, SpeedPresetsAndSliderHaveSeparateTargets) {
    const auto bounds = D2D1::RectF(0, 0, 280, 340);
    TEST_ASSERT_EQ(PlaybackFlyoutLayout::HitTest("speed", 9, bounds, 140, 95, 1), PlaybackFlyoutLayout::None);
    TEST_ASSERT(PtInRectF(PlaybackFlyoutLayout::SpeedTrack(bounds, 1), 140, 95));
    TEST_ASSERT_EQ(PlaybackFlyoutLayout::HitTest("speed", 9, bounds, 30, 120, 1), 0);
    TEST_ASSERT_EQ(PlaybackFlyoutLayout::HitTest("speed", 9, bounds, 97, 120, 1), PlaybackFlyoutLayout::None);
    TEST_ASSERT_EQ(PlaybackFlyoutLayout::HitTest("speed", 9, bounds, 30, 140, 1), PlaybackFlyoutLayout::None);
    TEST_ASSERT_EQ(PlaybackFlyoutLayout::HitTest("speed", 9, bounds, 250, 190, 1), 8);
    TEST_ASSERT_EQ(PlaybackFlyoutLayout::HitTest("speed", 9, bounds, 275, 120, 1), PlaybackFlyoutLayout::None);
}

TEST_CASE(PlaybackFlyout, InvisibleTrackRowsCannotReceiveInput) {
    const auto bounds = D2D1::RectF(0, 0, 280, 340);
    TEST_ASSERT_EQ(PlaybackFlyoutLayout::HitTest("audio", 100, bounds, 140, 350, 1), PlaybackFlyoutLayout::None);
    TEST_ASSERT_EQ(PlaybackFlyoutLayout::HitTest("subtitles", 0, bounds, 140, 70, 1), PlaybackFlyoutLayout::None);
}

TEST_CASE(PlaybackFlyout, NumericSpeedEntryRejectsInvalidOrPartialValues) {
    double speed = -1;
    TEST_ASSERT(PlaybackFlyoutLayout::ParseSpeed(L"1.25", speed)); TEST_ASSERT_EQ(speed, 1.25);
    TEST_ASSERT(PlaybackFlyoutLayout::ParseSpeed(L" 1,5x ", speed)); TEST_ASSERT_EQ(speed, 1.5);
    TEST_ASSERT(PlaybackFlyoutLayout::ParseSpeed(L"0.25", speed)); TEST_ASSERT_EQ(speed, 0.25);
    TEST_ASSERT(PlaybackFlyoutLayout::ParseSpeed(L"4.0", speed)); TEST_ASSERT_EQ(speed, 4.0);
    for (const wchar_t* text : { L"", L" ", L"nan", L"inf", L"0", L"-1", L"0.24", L"4.01", L"1.2oops", L"1,2,3", L"1x junk" }) {
        speed = -1;
        TEST_ASSERT(!PlaybackFlyoutLayout::ParseSpeed(text, speed));
        TEST_ASSERT_EQ(speed, -1.0);
    }
    TEST_ASSERT(!PlaybackFlyoutLayout::ParseSpeed(std::wstring(L"1\0junk", 6), speed));
}

} // namespace Aperture::Test
