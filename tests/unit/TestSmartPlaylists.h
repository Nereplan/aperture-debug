#pragma once

#include "../support/TestFramework.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"
#include "../../src/PreparedSmartPlaylist.h"
#include <string>
#include <vector>

namespace Aperture::Test {

TEST_CASE(SmartPlaylists, RuleMatchingByTitleAndPath) {
    MediaItem item;
    item.id = "item_1";
    item.title = L"Avengers Endgame 2019";
    item.filePath = L"C:\\Media\\Movies\\Avengers.mkv";

    SmartPlaylistRule titleRule;
    titleRule.field = SmartRuleField::Title;
    titleRule.op = SmartRuleOperator::Contains;
    titleRule.value = "endgame";
    TEST_ASSERT(titleRule.Matches(item));

    titleRule.value = "batman";
    TEST_ASSERT(!titleRule.Matches(item));

    SmartPlaylistRule pathRule;
    pathRule.field = SmartRuleField::Path;
    pathRule.op = SmartRuleOperator::EndsWith;
    pathRule.value = ".mkv";
    TEST_ASSERT(pathRule.Matches(item));

    pathRule.value = ".mp4";
    TEST_ASSERT(!pathRule.Matches(item));
}

TEST_CASE(SmartPlaylists, RuleMatchingByStatusDurationAndProgress) {
    MediaItem item;
    item.id = "item_2";
    item.title = L"Tutorial Episode 1";
    item.filePath = L"C:\\Media\\Tutorial.mp4";
    item.status = "in_progress";
    item.durationSec = 1800.0; // 30 mins
    item.lastPosSec = 900.0;   // 50%

    SmartPlaylistRule statusRule;
    statusRule.field = SmartRuleField::Status;
    statusRule.op = SmartRuleOperator::Equals;
    statusRule.value = "in_progress";
    TEST_ASSERT(statusRule.Matches(item));

    statusRule.value = "done";
    TEST_ASSERT(!statusRule.Matches(item));

    SmartPlaylistRule durRule;
    durRule.field = SmartRuleField::Duration;
    durRule.op = SmartRuleOperator::GreaterThan;
    durRule.value = "20m"; // 1200s
    TEST_ASSERT(durRule.Matches(item));

    durRule.value = "40m"; // 2400s
    TEST_ASSERT(!durRule.Matches(item));

    SmartPlaylistRule progRule;
    progRule.field = SmartRuleField::Progress;
    progRule.op = SmartRuleOperator::GreaterThanOrEqual;
    progRule.value = "0.5";
    TEST_ASSERT(progRule.Matches(item));
}

TEST_CASE(SmartPlaylists, CriteriaMatchAllAndMatchAny) {
    MediaItem item1;
    item1.title = L"Action Movie";
    item1.status = "new";
    item1.durationSec = 6000.0;

    SmartPlaylistCriteria critAll;
    critAll.matchMode = SmartMatchMode::MatchAll;
    critAll.rules.push_back({ SmartRuleField::Title, SmartRuleOperator::Contains, "Action" });
    critAll.rules.push_back({ SmartRuleField::Status, SmartRuleOperator::Equals, "new" });
    TEST_ASSERT(critAll.Matches(item1));

    critAll.rules.push_back({ SmartRuleField::Status, SmartRuleOperator::Equals, "done" });
    TEST_ASSERT(!critAll.Matches(item1));

    SmartPlaylistCriteria critAny;
    critAny.matchMode = SmartMatchMode::MatchAny;
    critAny.rules = critAll.rules;
    TEST_ASSERT(critAny.Matches(item1));
}

TEST_CASE(SmartPlaylists, DeterministicEvaluationAndProjection) {
    MediaLibrary lib;
    
    MediaItem a;
    a.id = "a";
    a.title = L"Beta Video";
    a.filePath = L"C:\\Media\\beta.mp4";
    a.status = "new";
    a.durationSec = 500.0;
    lib.mediaItems.push_back(a);

    MediaItem b;
    b.id = "b";
    b.title = L"Alpha Video";
    b.filePath = L"C:\\Media\\alpha.mp4";
    b.status = "new";
    b.durationSec = 1000.0;
    lib.mediaItems.push_back(b);

    MediaItem c;
    c.id = "c";
    c.title = L"Gamma Video";
    c.filePath = L"C:\\Media\\gamma.mp4";
    c.status = "done";
    c.durationSec = 200.0;
    lib.mediaItems.push_back(c);

    SmartPlaylistCriteria crit;
    crit.rules.push_back({ SmartRuleField::Status, SmartRuleOperator::Equals, "new" });
    crit.sortField = 0; // Title
    crit.sortDirection = 0; // Ascending

    std::vector<std::wstring> result = lib.EvaluateSmartPlaylist(crit);
    TEST_ASSERT_EQ(result.size(), 2);
    TEST_ASSERT_EQ(result[0], L"C:\\Media\\alpha.mp4");
    TEST_ASSERT_EQ(result[1], L"C:\\Media\\beta.mp4");
}

TEST_CASE(SmartPlaylists, SmartPlaylistLifecycleAndImmutability) {
    MediaLibrary lib;
    
    SmartPlaylistCriteria crit;
    crit.rules.push_back({ SmartRuleField::Title, SmartRuleOperator::Contains, "Test" });

    Playlist* smartPl = lib.CreateSmartPlaylist("My Smart Playlist", crit);
    TEST_ASSERT(smartPl != nullptr);
    TEST_ASSERT(smartPl->IsSmart());
    TEST_ASSERT_EQ(smartPl->type, PlaylistType::Smart);
    std::string plId = smartPl->id;

    // Direct item mutation should be rejected on smart playlists
    TEST_ASSERT(!lib.AddToPlaylist(plId, L"C:\\Media\\manual.mp4"));
    TEST_ASSERT(!lib.RemoveFromPlaylist(plId, 0));

    // Update criteria
    crit.rules.push_back({ SmartRuleField::Status, SmartRuleOperator::Equals, "new" });
    TEST_ASSERT(lib.UpdateSmartPlaylist(plId, "Renamed Smart Playlist", crit));
    
    Playlist* updated = lib.FindPlaylist(plId);
    TEST_ASSERT(updated != nullptr);
    TEST_ASSERT_EQ(updated->name, "Renamed Smart Playlist");
    TEST_ASSERT_EQ(updated->smartCriteria.rules.size(), 2);

    // Delete
    TEST_ASSERT(lib.DeletePlaylist(plId));
    TEST_ASSERT(lib.FindPlaylist(plId) == nullptr);
}

TEST_CASE(SmartPlaylists, SerializationAndUnknownFieldsPreservation) {
    Playlist pl;
    pl.id = "pl_smart_test";
    pl.name = "Smart Unwatched";
    pl.type = PlaylistType::Smart;
    pl.smartCriteria.rules.push_back({ SmartRuleField::Status, SmartRuleOperator::Equals, "new" });
    pl.smartCriteria.sortField = 1; // DateAdded
    pl.smartCriteria.sortDirection = 1; // Descending
    pl.extraFields = JsonValue::Object();
    pl.extraFields["customAnnotation"] = "User note";

    JsonValue serialized = pl.extraFields;
    serialized["id"] = pl.id;
    serialized["name"] = pl.name;
    serialized["createdTimestamp"] = pl.createdTimestamp;
    serialized["type"] = pl.IsSmart() ? "smart" : "manual";
    serialized["isSmart"] = pl.IsSmart();
    serialized["smartCriteria"] = pl.smartCriteria.ToJson();

    std::string jsonStr = serialized.stringify(false);

    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse(jsonStr, parsed));
    TEST_ASSERT_EQ(parsed.get("id").as_string(), "pl_smart_test");
    TEST_ASSERT_EQ(parsed.get("type").as_string(), "smart");
    TEST_ASSERT_EQ(parsed.get("isSmart").as_bool(), true);
    TEST_ASSERT_EQ(parsed.get("customAnnotation").as_string(), "User note");

    SmartPlaylistCriteria loadedCrit = SmartPlaylistCriteria::FromJson(parsed.get("smartCriteria"));
    TEST_ASSERT_EQ(loadedCrit.rules.size(), 1);
    TEST_ASSERT_EQ(loadedCrit.rules[0].value, "new");
    TEST_ASSERT_EQ(loadedCrit.sortField, 1);
    TEST_ASSERT_EQ(loadedCrit.sortDirection, 1);
}

TEST_CASE(SmartPlaylists, ProgressRefreshesPathsAndStatsWithoutStatusChange) {
    MediaLibrary library;
    MediaItem item;
    item.id = "progress-item";
    item.filePath = L"C:\\Media\\progress.mkv";
    item.title = L"Progress";
    item.durationSec = 100;
    item.lastPosSec = 10;
    item.status = "in_progress";
    library.mediaItems.push_back(item);
    Playlist playlist;
    playlist.id = "progress-playlist";
    playlist.type = PlaylistType::Smart;
    SmartPlaylistRule rule;
    rule.field = SmartRuleField::Progress;
    rule.op = SmartRuleOperator::GreaterThanOrEqual;
    rule.value = "0.5";
    playlist.smartCriteria.rules.push_back(rule);
    library.playlists.push_back(playlist);
    const auto& stored = library.playlists.front();
    TEST_ASSERT(library.GetPlaylistPathsRef(stored).empty());
    TEST_ASSERT_EQ(library.GetPlaylistStats(stored.id).totalCount, 0);
    library.UpdateProgress(item.filePath, 60, 100);
    TEST_ASSERT_EQ(library.GetPlaylistPathsRef(stored).size(), size_t(1));
    TEST_ASSERT_EQ(library.GetPlaylistPaths(stored).size(), size_t(1));
    TEST_ASSERT_EQ(library.GetPlaylistStats(stored.id).totalCount, 1);
    library.UpdateProgress(item.filePath, 20, 100);
    TEST_ASSERT(library.GetPlaylistPathsRef(stored).empty());
    TEST_ASSERT_EQ(library.GetPlaylistStats(stored.id).totalCount, 0);
}

TEST_CASE(SmartPlaylists, LimitedEvaluationMatchesUnlimitedPrefix) {
    MediaLibrary library;
    for (int i = 199; i >= 0; --i) {
        MediaItem item;
        item.id = std::to_string(i / 2);
        item.filePath = L"C:\\Media\\Video" + std::to_wstring(i / 2) + L".mkv";
        item.title = L"Video " + std::to_wstring(i % 9);
        item.addedTimestamp = i / 4;
        item.lastWatchedTimestamp = i / 6;
        item.durationSec = 100 + i / 8;
        item.lastPosSec = i / 10;
        library.mediaItems.push_back(std::move(item));
    }
    for (int field = 0; field <= 5; ++field) {
        for (int direction = 0; direction <= 1; ++direction) {
            SmartPlaylistCriteria criteria;
            criteria.sortField = field;
            criteria.sortDirection = direction;
            const auto full = library.EvaluateSmartPlaylist(criteria);
            for (size_t limit : {size_t(1), size_t(2), size_t(50), size_t(199), size_t(200), size_t(201)}) {
                criteria.limit = limit;
                const auto paths = library.EvaluateSmartPlaylist(criteria);
                TEST_ASSERT_EQ(paths.size(), (std::min)(limit, full.size()));
                for (size_t i = 0; i < paths.size(); ++i) TEST_ASSERT_EQ(paths[i], full[i]);
            }
        }
    }
}

TEST_CASE(SmartPlaylists, LimitedSparseMatchesFollowNumericOracle) {
    MediaLibrary library;
    for (int i = 4095; i >= 0; --i) {
        MediaItem item;
        item.id = std::to_string(i);
        item.filePath = L"C:\\Media\\" + std::to_wstring(i) + L".mkv";
        item.durationSec = (i * 137) % 4096 + 0.5;
        library.mediaItems.push_back(std::move(item));
    }
    for (size_t matchCount : {size_t(0), size_t(3), size_t(17), size_t(147), size_t(4096)}) {
        std::vector<std::pair<double, std::wstring>> oracle;
        for (size_t i = 0; i < library.mediaItems.size(); ++i) {
            auto& item = library.mediaItems[i];
            item.title = i < matchCount ? L"selected" : L"excluded";
            if (i < matchCount) oracle.emplace_back(item.durationSec, item.filePath);
        }
        for (int direction = 0; direction <= 1; ++direction) {
            std::sort(oracle.begin(), oracle.end(), [direction](const auto& a, const auto& b) {
                return direction ? a.first > b.first : a.first < b.first;
            });
            SmartPlaylistCriteria criteria;
            criteria.rules = {{SmartRuleField::Title, SmartRuleOperator::Equals, "selected"}};
            criteria.sortField = 3; // Duration
            criteria.sortDirection = direction;
            for (size_t limit : {size_t(1), size_t(2), size_t(17), size_t(147), size_t(1023), size_t(1024), size_t(4097)}) {
                criteria.limit = limit;
                const auto paths = library.EvaluateSmartPlaylist(criteria);
                TEST_ASSERT_EQ(paths.size(), (std::min)(limit, oracle.size()));
                for (size_t i = 0; i < paths.size(); ++i) TEST_ASSERT_EQ(paths[i], oracle[i].second);
            }
        }
    }
}

TEST_CASE(SmartPlaylists, LargeLimitMatchesNumericOracle) {
    MediaLibrary library;
    std::vector<size_t> oracle;
    for (size_t i = 0; i < 100000; ++i) {
        MediaItem item;
        item.id = std::to_string(i);
        item.filePath = L"C:\\Media\\" + std::to_wstring(i) + L".mkv";
        item.durationSec = (i * 193) % 100000 + 0.5;
        library.mediaItems.push_back(std::move(item));
        oracle.push_back(i);
    }
    for (int direction = 0; direction <= 1; ++direction) {
        std::sort(oracle.begin(), oracle.end(), [&](size_t a, size_t b) {
            return direction ? library.mediaItems[a].durationSec > library.mediaItems[b].durationSec
                : library.mediaItems[a].durationSec < library.mediaItems[b].durationSec;
        });
        SmartPlaylistCriteria criteria;
        criteria.sortField = 3; // Duration
        criteria.sortDirection = direction;
        for (size_t limit : {size_t(24000), size_t(24999), size_t(25000)}) {
            criteria.limit = limit;
            const auto paths = library.EvaluateSmartPlaylist(criteria);
            TEST_ASSERT_EQ(paths.size(), limit);
            for (size_t i = 0; i < paths.size(); ++i) TEST_ASSERT_EQ(paths[i], library.mediaItems[oracle[i]].filePath);
        }
    }
}


TEST_CASE(SmartPlaylists, PathGenerationTracksRefreshesWithoutUnrelatedEdits) {
    MediaLibrary library;
    library.SetSaveRequestHandler([] { return MediaLibrarySaveResult{}; });
    MediaItem item; item.id = "one"; item.title = L"One"; item.filePath = LR"(C:\Media\one.mkv)";
    item.durationSec = 100; library.mediaItems.push_back(item); library.RebuildPathIndex();
    Playlist playlist; playlist.id = "smart"; playlist.type = PlaylistType::Smart;
    SmartPlaylistRule rule; rule.field = SmartRuleField::Status; rule.op = SmartRuleOperator::Equals; rule.value = "new";
    playlist.smartCriteria.rules.push_back(rule); library.playlists.push_back(playlist); library.NotifyPlaylistEdited();
    const auto first = library.ResolvePlaylistPaths(library.playlists.front());
    TEST_ASSERT_EQ(first.paths.size(), size_t(1));
    const auto generation = first.generation;
    library.AddToHistory(item.filePath, 0, 100);
    TEST_ASSERT_EQ(library.ResolvePlaylistPaths(library.playlists.front()).generation, generation);
    library.InvalidatePlaylistStatsCache();
    const auto refreshed = library.ResolvePlaylistPaths(library.playlists.front()).generation;
    TEST_ASSERT(refreshed > generation);
    library.SetWatched(item.filePath, true);
    const auto watched = library.ResolvePlaylistPaths(library.playlists.front());
    TEST_ASSERT(watched.paths.empty());
    TEST_ASSERT(watched.generation > refreshed);
}

TEST_CASE(SmartPlaylists, PreparedRulesPreserveExistingOperatorSemantics) {
    MediaItem item;
    item.title = L"Mixed Film"; item.filePath = LR"(C:\Media\Mixed Film.mkv)";
    item.status = "in_progress"; item.durationSec = 120; item.lastPosSec = 30;
    item.fileSizeBytes = 4096; item.addedTimestamp = 100; item.lastWatchedTimestamp = 200; item.videoHeight = 1080;
    const std::vector<std::pair<SmartRuleField, std::string>> cases = {
        {SmartRuleField::Title, "MIXED"}, {SmartRuleField::Path, "Media"}, {SmartRuleField::Status, " watching "},
        {SmartRuleField::Status, "missing"}, {SmartRuleField::Status, "completed"}, {SmartRuleField::Duration, "2m"},
        {SmartRuleField::Progress, "25%"}, {SmartRuleField::FileSize, "4kb"}, {SmartRuleField::DateAdded, "100"},
        {SmartRuleField::LastWatched, "200"}, {SmartRuleField::VideoHeight, "fhd"}
    };
    for (const auto& value : cases) for (int op = 0; op <= static_cast<int>(SmartRuleOperator::EndsWith); ++op) {
        SmartPlaylistCriteria criteria;
        SmartPlaylistRule rule; rule.field = value.first; rule.op = static_cast<SmartRuleOperator>(op); rule.value = value.second;
        criteria.rules.push_back(rule);
        PreparedSmartPlaylist plan(criteria, 1000);
        TEST_ASSERT(plan.IsValid());
        const auto result = plan.MatchWithoutDisk(item);
        TEST_ASSERT_EQ(result.present, criteria.Matches(item, true));
        TEST_ASSERT_EQ(result.missing, criteria.Matches(item, false));
    }
    for (auto mode : {SmartMatchMode::MatchAll, SmartMatchMode::MatchAny}) {
        SmartPlaylistCriteria criteria; criteria.matchMode = mode;
        criteria.rules = {{SmartRuleField::Title, SmartRuleOperator::Contains, "absent"},
                          {SmartRuleField::Status, SmartRuleOperator::Equals, "missing"}};
        const auto result = PreparedSmartPlaylist(criteria, 1000).MatchWithoutDisk(item);
        TEST_ASSERT_EQ(result.present, criteria.Matches(item, true));
        TEST_ASSERT_EQ(result.missing, criteria.Matches(item, false));
    }
}

TEST_CASE(SmartPlaylists, RelativeRulesShareClockAndExposeMembershipBoundaries) {
    const int64_t now = 200000;
    MediaItem item; item.addedTimestamp = now - 86400 + 10;
    SmartPlaylistCriteria criteria;
    criteria.rules = {{SmartRuleField::DateAdded, SmartRuleOperator::GreaterThan, "1d"}};
    const PreparedSmartPlaylist before(criteria, now);
    TEST_ASSERT(before.DependsOnTime());
    TEST_ASSERT(before.MatchWithoutDisk(item).present);
    TEST_ASSERT_EQ(before.NextChangeAfter(item), now + 10);
    const PreparedSmartPlaylist boundary(criteria, now + 10);
    TEST_ASSERT(!boundary.MatchWithoutDisk(item).present);
    TEST_ASSERT_EQ(boundary.NextChangeAfter(item), now + 11);
    criteria.rules.front().op = SmartRuleOperator::Equals;
    TEST_ASSERT(PreparedSmartPlaylist(criteria, now + 10).MatchWithoutDisk(item).present);
    TEST_ASSERT(!PreparedSmartPlaylist(criteria, now + 11).MatchWithoutDisk(item).present);
    TEST_ASSERT_EQ(PreparedSmartPlaylist(criteria, now + 11).NextChangeAfter(item), (std::numeric_limits<int64_t>::max)());
}

TEST_CASE(SmartPlaylists, PlaybackIndependentMembershipAndRejectedDiskCandidatesAreReused) {
    MediaLibrary library;
    library.SetSaveRequestHandler([] { return MediaLibrarySaveResult{}; });
    MediaItem item; item.id = "episode"; item.title = L"Episode"; item.filePath = LR"(C:\Media\episode.mkv)";
    item.durationSec = 100; library.mediaItems.push_back(item); library.RebuildPathIndex();
    Playlist playlist; playlist.id = "titles"; playlist.type = PlaylistType::Smart;
    playlist.smartCriteria.rules = {{SmartRuleField::Title, SmartRuleOperator::Contains, "episode"}};
    library.playlists.push_back(playlist); library.NotifyPlaylistEdited();
    const auto generation = library.ResolvePlaylistPaths(library.playlists.front()).generation;
    const auto evaluations = library.GetSmartPlaylistEvaluationCount();
    library.UpdateProgress(item.filePath, 25, 100);
    TEST_ASSERT_EQ(library.ResolvePlaylistPaths(library.playlists.front()).generation, generation);
    TEST_ASSERT_EQ(library.GetSmartPlaylistEvaluationCount(), evaluations);
    SmartPlaylistCriteria impossible;
    impossible.rules = {{SmartRuleField::Title, SmartRuleOperator::Equals, "absent"},
                        {SmartRuleField::Status, SmartRuleOperator::Equals, "missing"}};
    const auto probes = library.GetSmartPlaylistDiskProbeCount();
    TEST_ASSERT(library.EvaluateSmartPlaylist(impossible).empty());
    TEST_ASSERT_EQ(library.GetSmartPlaylistDiskProbeCount(), probes);
}

} // namespace Aperture::Test
