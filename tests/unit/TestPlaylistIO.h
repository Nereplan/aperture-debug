#pragma once

#include "../support/TestFramework.h"
#include "../../src/PlaylistIO.h"
#include <random>

namespace Aperture::Test {

TEST_CASE(PlaylistIO, BatchAppendUsesOrdinalIgnoreCaseAndKeepsFirstOrder) {
    std::vector<std::wstring> existing = {
        L"C:\\Media\\Existing.mp4", L"C:\\Media\\Other.mp4", L"C:\\Media\\existing.mp4"
    };
    const std::vector<std::wstring> candidates = {
        L"c:\\media\\EXISTING.MP4", L"C:\\Media\\New.mp4", L"c:\\media\\new.MP4",
        L"C:\\Media\\Last.mp4", L"C:\\Media\\OTHER.mp4", L"C:\\Media\\New.mp4",
        L"C:\\Media\\Last.mp4", L"C:\\Media\\OTHER.mp4", L"C:\\Media\\New.mp4"
    };

    const size_t added = PlaylistIO::AppendUniquePathsOrdinalIgnoreCase(existing, candidates);
    TEST_ASSERT_EQ(added, size_t(2));
    TEST_ASSERT_EQ(existing.size(), size_t(5));
    TEST_ASSERT_EQ(existing[0], std::wstring(L"C:\\Media\\Existing.mp4"));
    TEST_ASSERT_EQ(existing[1], std::wstring(L"C:\\Media\\Other.mp4"));
    TEST_ASSERT_EQ(existing[2], std::wstring(L"C:\\Media\\existing.mp4"));
    TEST_ASSERT_EQ(existing[3], std::wstring(L"C:\\Media\\New.mp4"));
    TEST_ASSERT_EQ(existing[4], std::wstring(L"C:\\Media\\Last.mp4"));
}

TEST_CASE(PlaylistIO, BatchAppendUsesOrdinalCaseRulesForUnicode) {
    std::vector<std::wstring> existing = {L"C:\\Media\\\u00E9.mov"};
    const std::vector<std::wstring> candidates = {
        L"c:\\media\\\u00C9.MOV", L"C:\\Media\\e\u0301.mov", L"C:\\Media\\\u00D6.mov",
        L"c:\\media\\\u00C9.MOV", L"C:\\Media\\e\u0301.mov", L"C:\\Media\\\u00F6.MOV",
        L"c:\\media\\\u00C9.MOV", L"C:\\Media\\e\u0301.mov", L"C:\\Media\\\u00D6.mov"
    };

    const size_t added = PlaylistIO::AppendUniquePathsOrdinalIgnoreCase(existing, candidates);
    TEST_ASSERT_EQ(added, size_t(2));
    TEST_ASSERT_EQ(existing.size(), size_t(3));
    TEST_ASSERT_EQ(existing[0], std::wstring(L"C:\\Media\\\u00E9.mov"));
    TEST_ASSERT_EQ(existing[1], std::wstring(L"C:\\Media\\e\u0301.mov"));
    TEST_ASSERT_EQ(existing[2], std::wstring(L"C:\\Media\\\u00D6.mov"));
}

TEST_CASE(PlaylistIO, BatchAppendMatchesSequentialMembershipOracle) {
    std::mt19937 random(20261001);
    for (size_t count : {size_t(0), size_t(1), size_t(8), size_t(9), size_t(256)}) {
        for (int sample = 0; sample < 12; ++sample) {
            auto path = [&]() {
                const auto id = random() % 100;
                return (random() % 2 ? L"C:\\MEDIA\\Video" : L"c:\\media\\video") +
                    std::to_wstring(id) + (random() % 2 ? L".MKV" : L".mkv");
            };
            std::vector<std::wstring> existing;
            const size_t existingCount = random() % 128;
            for (size_t i = 0; i < existingCount; ++i) existing.push_back(path());
            std::vector<std::wstring> candidates;
            for (size_t i = 0; i < count; ++i) candidates.push_back(path());
            auto expected = existing;
            for (const auto& candidate : candidates) {
                bool duplicate = false;
                for (const auto& prior : expected) {
                    if (CompareStringOrdinal(prior.c_str(), -1, candidate.c_str(), -1, TRUE) == CSTR_EQUAL) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) expected.push_back(candidate);
            }
            const auto expectedAdded = expected.size() - existing.size();
            TEST_ASSERT_EQ(PlaylistIO::AppendUniquePathsOrdinalIgnoreCase(existing, candidates), expectedAdded);
            TEST_ASSERT_EQ(existing.size(), expected.size());
            for (size_t i = 0; i < existing.size(); ++i) TEST_ASSERT_EQ(existing[i], expected[i]);
        }
    }
    // A repeated candidate after every existing group previously could cause
    // repeated scans of the same duplicate group during the merge.
    std::vector<std::wstring> existing;
    for (int i = 0; i < 100; ++i) existing.push_back(L"A" + std::to_wstring(i));
    const std::vector<std::wstring> repeated(256, L"Z");
    TEST_ASSERT_EQ(PlaylistIO::AppendUniquePathsOrdinalIgnoreCase(existing, repeated), size_t(1));
    TEST_ASSERT_EQ(existing.back(), std::wstring(L"Z"));
}

} // namespace Aperture::Test
