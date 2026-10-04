#include "MediaLibrary.h"
#include "Settings.h"
#include "BackupFiles.h"
#include <iostream>
#include <stdexcept>

using namespace Aperture;
static void Check(bool value) { if (!value) throw std::runtime_error("probe setup failed"); }
int main() {
    // The runner builds this executable in a fresh temporary directory.
    const auto root = GetExeDir();
    const auto data = GetDataDir();
    const auto libPath = data + L"\\library.json";
    Check(AtomicWriteTextFile(libPath, "{\"schemaVersion\":2,\"items\":{\"unexpected\":true}}").is_ok());
    MediaLibrary malformedShape;
    const auto shapeStatus = malformedShape.LoadFromRoot(root);
    std::cout << "wrong_library_shape load_ok=" << shapeStatus.is_ok()
              << " items=" << malformedShape.mediaItems.size() << '\n';

    MediaLibrary unavailable;
    MediaItem item; item.id = "offline-item";
    const auto missingRoot = root + L"\\unavailable-drive";
    item.filePath = missingRoot + L"\\movie.mkv";
    item.title = L"Retained metadata"; item.durationSec = 120; item.lastPosSec = 42;
    unavailable.mediaItems.push_back(item);
    Playlist list; list.id = "playlist"; list.name = "Playlist"; list.itemPaths.push_back(item.filePath);
    unavailable.playlists.push_back(list); unavailable.RebuildPathIndex();
    const auto rescan = unavailable.ReconcileRescan(missingRoot, false);
    std::string disk; Check(ReadTextFile(libPath, disk).is_ok());
    std::cout << "unavailable_root removed=" << rescan.removedCount
              << " remaining_items=" << unavailable.mediaItems.size()
              << " remaining_playlist_paths=" << unavailable.playlists.front().itemPaths.size()
              << " saveChanges_false_still_wrote=" << (disk == "[]") << '\n';

    for (auto field : {SmartRuleField::Duration, SmartRuleField::DateAdded}) {
        SmartPlaylistRule rule; rule.field = field; rule.op = SmartRuleOperator::GreaterThan;
        rule.value = field == SmartRuleField::Duration ? "badm" : "bad days";
        bool threw = false;
        try { (void)rule.Matches(item); } catch (const std::exception&) { threw = true; }
        std::cout << "malformed_smart_rule field=" << static_cast<int>(field) << " threw=" << threw << '\n';
    }

    MediaLibrary smartMissing; smartMissing.mediaItems.push_back(item); smartMissing.RebuildPathIndex();
    SmartPlaylistCriteria criteria; SmartPlaylistRule missingRule;
    missingRule.field = SmartRuleField::Status; missingRule.op = SmartRuleOperator::Equals; missingRule.value = "missing";
    criteria.rules.push_back(missingRule);
    std::cout << "smart_missing_rule nonexistent_item_matches=" << smartMissing.EvaluateSmartPlaylist(criteria).size() << '\n';

    const auto zeroDurationFile = root + L"\\zero-duration.mkv";
    Check(AtomicWriteTextFile(zeroDurationFile, "probe fixture").is_ok());
    MediaLibrary backfill; item.filePath = zeroDurationFile; item.durationSec = 0;
    backfill.mediaItems.push_back(item); backfill.RebuildPathIndex();
    MetadataBackfillOptions options;
    options.probeFunction = [](const std::wstring&, double& d, uint32_t& w, uint32_t& h) { d = 0; w = h = 0; return true; };
    const auto first = backfill.BackfillMissingMetadata(options, {}, false);
    const auto second = backfill.BackfillMissingMetadata(options, {}, false);
    std::cout << "zero_duration_backfill first_completed=" << first.completedCount << " second_completed=" << second.completedCount
              << " attempts=" << backfill.mediaItems.front().probeAttemptCount << '\n';

    const auto backupSource = root + L"\\large.json";
    Check(AtomicWriteTextFile(backupSource, std::string(33 * 1024 * 1024, 'x')).is_ok());
    const auto backup = CreateRotatingBackup(backupSource);
    std::cout << "large_backup status_ok=" << backup.is_ok()
              << " returned_backup_exists=" << FileExists(backup.path) << '\n';

    const auto settingsPath = data + L"\\settings.json";
    Check(AtomicWriteTextFile(settingsPath, "{\"themePreset\":\"sunset\",BROKEN").is_ok());
    Settings settings;
    const auto load = settings.Load();
    const auto save = settings.Save();
    Check(ReadTextFile(settingsPath, disk).is_ok()); JsonValue parsed;
    std::cout << "failed_settings_load load_ok=" << load.is_ok() << " subsequent_save_ok=" << save.is_ok()
              << " malformed_source_replaced=" << JsonValue::parse(disk, parsed) << '\n';

    settings.volume = 0.25;
    Check(settings.Save().is_ok());
    Settings reloadedSettings; Check(reloadedSettings.Load().is_ok());
    std::cout << "volume_roundtrip requested=" << settings.volume << " reloaded=" << reloadedSettings.volume << '\n';

    MediaLibrary promoted;
    MediaItem newer; newer.id = "new-file"; newer.filePath = zeroDurationFile; newer.lastWatchedTimestamp = 0;
    MediaItem older; older.id = "previous-file"; older.filePath = root + L"\\previous.mkv";
    older.lastWatchedTimestamp = 100; older.lastPosSec = 42;
    promoted.mediaItems = {newer, older}; promoted.RebuildPathIndex();
    auto* borrowed = promoted.FindByPath(newer.filePath);
    const auto beforeId = borrowed->id;
    Check(promoted.PromoteMostRecentlyWatched());
    std::cout << "borrowed_media_pointer before=" << beforeId << " after=" << borrowed->id
              << " wrong_resume_position=" << borrowed->lastPosSec << '\n';
}
