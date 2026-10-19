#pragma once
#include "../support/TestFramework.h"
#include "../../src/Presentation.h"

namespace Aperture::Test {
TEST_CASE(Presentation, NavigationFitsAndMirrorsAtMinimumHubWidth) {
    for (float dp : {1.0f, 1.25f, 1.5f, 2.0f}) {
        for (float glyphWidth : {8.0f, 20.0f}) {
            const float width = 640.0f * dp;
            const auto measure = [=](const std::wstring& text) { return text.size() * glyphWidth * dp; };
            const auto ltr = Presentation::Nav(width, dp, false, measure);
            const auto rtl = Presentation::Nav(width, dp, true, measure);
            TEST_ASSERT(ltr.track.right < ltr.settings.left);
            TEST_ASSERT(ltr.settings.right < ltr.open.left);
            for (int i = 0; i < 3; ++i) {
                TEST_ASSERT(ltr.tabs[i].left < ltr.tabs[i].right);
                TEST_ASSERT(std::abs(rtl.tabs[i].left - (width - ltr.tabs[i].right)) < 0.001f);
                TEST_ASSERT(std::abs(rtl.tabs[i].right - (width - ltr.tabs[i].left)) < 0.001f);
                if (i) TEST_ASSERT(ltr.tabs[i - 1].right < ltr.tabs[i].left);
            }
        }
    }
}

TEST_CASE(Presentation, ResponsiveControlsDoNotOverlapAtSupportedWidths) {
    for (float dp : {1.0f, 1.25f, 1.5f, 2.0f}) {
        for (float width : {400.0f, 519.0f, 520.0f, 640.0f, 719.0f, 720.0f, 1024.0f}) {
            const auto l = Presentation::Player(width * dp, 640 * dp, dp);
            TEST_ASSERT(l.play.right < l.next.left);
            TEST_ASSERT(l.next.right < l.mute.left);
            TEST_ASSERT(l.mute.right < l.settings.left);
            TEST_ASSERT(l.settings.right < l.fullscreen.left);
            TEST_ASSERT(l.play.top == l.settings.top);
            TEST_ASSERT_EQ(l.showPip, width >= 520);
            TEST_ASSERT_EQ(l.showVolume, width >= 720);
            TEST_ASSERT_EQ(l.showSubtitles, width >= 720);
            if (l.showPip) TEST_ASSERT(l.settings.right < l.pip.left);
            else TEST_ASSERT(l.pip.right == l.pip.left);
            if (l.showVolume) TEST_ASSERT(l.volume.right < l.subtitles.left);
            else TEST_ASSERT(l.volume.right == l.volume.left);
        }
    }
}

TEST_CASE(Presentation, ButtonTextAdaptsToLightAndDarkCustomAccents) {
    ThemePalette theme{};
    theme.accent = D2D1::ColorF(0.95f, 0.95f, 0.95f);
    auto light = Presentation::Button(theme, true, false, false, false);
    TEST_ASSERT(light.text.r == 0.0f);
    theme.accent = D2D1::ColorF(0.05f, 0.05f, 0.10f);
    auto dark = Presentation::Button(theme, true, false, true, false);
    TEST_ASSERT(dark.text.r == 1.0f);
    theme.red = D2D1::ColorF(0.7f, 0.1f, 0.1f);
    auto danger = Presentation::Button(theme, true, true, false, false);
    TEST_ASSERT(danger.fill.r == theme.red.r);
    TEST_ASSERT(danger.fill.r != dark.fill.r);
}

TEST_CASE(Presentation, StorageRecoveryDistinguishesDiskFullFromPermissions) {
    auto disk = StorageStatus::Error(StorageErrorCode::FileWriteFailed, ERROR_DISK_FULL, L"");
    auto denied = StorageStatus::Error(StorageErrorCode::DirectoryNotWritable, ERROR_ACCESS_DENIED, L"");
    auto read = StorageStatus::Error(StorageErrorCode::FileReadFailed, ERROR_READ_FAULT, L"");
    TEST_ASSERT(Presentation::StorageGuidance(disk) != Presentation::StorageGuidance(denied));
    TEST_ASSERT(Presentation::StorageGuidance(read) != Presentation::StorageGuidance(denied));
}

TEST_CASE(Presentation, QueueEndIsExplicitAndDirectOpenHasLibraryLabel) {
    TEST_ASSERT(Presentation::NextLabel(true, 0, 3) != Presentation::NextLabel(true, 2, 3));
    TEST_ASSERT(Presentation::NextLabel(false, 0, 0) != Presentation::NextLabel(true, 0, 3));
}
}
