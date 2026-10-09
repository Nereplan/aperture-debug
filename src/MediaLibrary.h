#pragma once

#include "Common.h"
#include "Json.h"
#include <string>
#include <string_view>
#include "FolderWatcher.h"
#include <vector>
#include <set>
#include <memory>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <optional>
#include <thread>

namespace Aperture {

class PreparedSmartPlaylist;
class MediaImportProbeCache;

struct MediaPlaybackPreferences {
    std::optional<double> playbackSpeed;       // Valid range: [0.25, 4.0]
    std::optional<std::string> aspectRatio;    // Valid modes: "auto", "16:9", "4:3", "21:9", "fill", "cover"
    std::optional<int> audioTrackId;           // >= 0
    std::optional<std::wstring> audioTrackTitle;
    std::optional<std::string> audioTrackLang;
    std::optional<int> subtitleTrackId;        // >= 0 (0 = off)
    std::optional<std::wstring> subtitleTrackTitle;
    std::optional<std::string> subtitleTrackLang;
    std::optional<int> audioDelayMs;           // Valid range: [-5000, 5000]
    std::optional<int> subDelayMs;             // Valid range: [-10000, 10000]
    std::optional<bool> subtitlesVisible;      // Subtitle visibility flag
    std::optional<int> videoRotate;            // Valid: 0, 90, 180, 270
    std::optional<int> brightness;             // Valid range: [-100, 100]
    std::optional<int> contrast;               // Valid range: [-100, 100]
    std::optional<int> saturation;             // Valid range: [-100, 100]
    std::optional<int> gamma;                  // Valid range: [-100, 100]
    std::optional<int> hue;                    // Valid range: [-100, 100]
    std::optional<double> audioBalance;        // Valid range: [-1.0, 1.0]
    std::optional<std::string> audioChannelMap;// Valid modes: "auto", "stereo", "mono-left", "mono-right", "swap"
    JsonValue extraFields;                     // Preserves any unknown fields inside "preferences"
    bool HasAnyPreference() const {
        return playbackSpeed.has_value() || aspectRatio.has_value() || audioTrackId.has_value() || audioTrackTitle.has_value() ||
               audioTrackLang.has_value() || subtitleTrackId.has_value() || subtitleTrackTitle.has_value() ||
               subtitleTrackLang.has_value() || audioDelayMs.has_value() || subDelayMs.has_value() || subtitlesVisible.has_value() ||
               videoRotate.has_value() || brightness.has_value() || contrast.has_value() || saturation.has_value() || gamma.has_value() ||
               hue.has_value() || audioBalance.has_value() || audioChannelMap.has_value() ||
               (extraFields.is_object() && !extraFields.as_object().empty());
    }
    static bool IsValidPlaybackSpeed(double speed) { return std::isfinite(speed) && speed >= 0.25 && speed <= 4.0; }
    static bool IsValidAspectRatio(const std::string& mode) {
        return mode == "auto" || mode == "16:9" || mode == "4:3" || mode == "21:9" || mode == "fill" || mode == "cover";
    }
    static bool IsValidAudioDelayMs(int delay) { return delay >= -5000 && delay <= 5000; }
    static bool IsValidSubDelayMs(int delay) { return delay >= -10000 && delay <= 10000; }
    static bool IsValidVideoRotate(int rotate) { return rotate == 0 || rotate == 90 || rotate == 180 || rotate == 270; }
    static bool IsValidBrightness(int b) { return b >= -100 && b <= 100; }
    static bool IsValidBrightness(double b) { return std::isfinite(b) && b >= -100.0 && b <= 100.0; }
    static bool IsValidContrast(int c) { return c >= -100 && c <= 100; }
    static bool IsValidContrast(double c) { return std::isfinite(c) && c >= -100.0 && c <= 100.0; }
    static bool IsValidSaturation(int s) { return s >= -100 && s <= 100; }
    static bool IsValidSaturation(double s) { return std::isfinite(s) && s >= -100.0 && s <= 100.0; }
    static bool IsValidGamma(int g) { return g >= -100 && g <= 100; }
    static bool IsValidGamma(double g) { return std::isfinite(g) && g >= -100.0 && g <= 100.0; }
    static bool IsValidHue(int h) { return h >= -100 && h <= 100; }
    static bool IsValidHue(double h) { return std::isfinite(h) && h >= -100.0 && h <= 100.0; }
    static bool IsValidAudioBalance(double b) { return std::isfinite(b) && b >= -1.0 && b <= 1.0; }
    static bool IsValidAudioChannelMap(const std::string& mode) {
        return mode == "auto" || mode == "stereo" || mode == "mono-left" || mode == "mono-right" || mode == "swap";
    }
};

struct MediaBookmark {
    static constexpr size_t kMaxLabelLength = 128;
    std::string id;
    double timeSec = 0.0;
    std::wstring label;
    int64_t createdTimestamp = 0;
    JsonValue extraFields;
    static bool IsValidTime(double t) { return std::isfinite(t) && t >= 0.0; }
    bool IsValid() const { return IsValidTime(timeSec); }
    static bool Compare(const MediaBookmark& a, const MediaBookmark& b) {
        if (a.timeSec != b.timeSec) return a.timeSec < b.timeSec;
        if (a.createdTimestamp != b.createdTimestamp) return a.createdTimestamp < b.createdTimestamp;
        if (a.label != b.label) return a.label < b.label;
        return a.id < b.id;
    }
};
struct MediaMetadataOverrides {
    static constexpr size_t kMaxTitleLength = 512, kMaxSeriesLength = 256;
    static constexpr int kMinYear = 1888, kMaxYear = 2100;
    std::optional<std::wstring> title;
    std::optional<int> year;
    std::optional<std::wstring> series;
    JsonValue extraFields;
    bool HasAnyOverride() const {
        return (title.has_value() && !title->empty()) || year.has_value() || (series.has_value() && !series->empty()) ||
               (extraFields.is_object() && !extraFields.as_object().empty());
    }
    static bool IsValidTitle(const std::wstring& t) { return !t.empty() && t.length() <= kMaxTitleLength; }
    static bool IsValidYear(int y) { return y >= kMinYear && y <= kMaxYear; }
    static bool IsValidSeries(const std::wstring& s) { return !s.empty() && s.length() <= kMaxSeriesLength; }
};

struct MediaItem {
    std::string id;
    std::wstring filePath, title;
    double durationSec = 0.0, lastPosSec = 0.0;
    std::string status = "new"; // "new", "in_progress", "done"
    uint64_t fileSizeBytes = 0;
    int64_t lastWatchedTimestamp = 0, addedTimestamp = 0;
    uint32_t videoWidth = 0, videoHeight = 0;
    uint64_t fileIndex = 0;
    uint32_t volumeSerialNumber = 0;
    uint64_t metadataFileWriteTime = 0; // FILETIME from the last stable, successful metadata probe.
    bool metadataProbeFailed = false;
    uint32_t probeAttemptCount = 0;
    int64_t lastProbeAttemptTimestamp = 0;
    std::optional<MediaPlaybackPreferences> preferences;
    std::vector<MediaBookmark> bookmarks;
    std::optional<MediaMetadataOverrides> overrides;
    JsonValue extraFields;
    double GetProgressRatio() const {
        if (status == "done") return 1.0;
        if (durationSec <= 0.0) return 0.0;
        return std::clamp(lastPosSec / durationSec, 0.0, 1.0);
    }
    // Borrowed title views are valid until this item is modified or moved.
    std::wstring_view GetTitleView() const {
        if (!title.empty()) return title;
        const size_t lastSlash = filePath.find_last_of(L"\\/");
        return std::wstring_view(filePath).substr(lastSlash == std::wstring::npos ? 0 : lastSlash + 1);
    }
    std::wstring_view GetDisplayTitleView() const {
        if (overrides.has_value() && overrides->title.has_value() && !overrides->title->empty()) { return *overrides->title; }
        return GetTitleView();
    }
    std::wstring GetDisplayTitle() const { return std::wstring(GetDisplayTitleView()); }
};

enum class SmartRuleField { Title = 0, Path, Status, Duration, Progress, FileSize, DateAdded, LastWatched, VideoHeight };

enum class SmartRuleOperator {
    Contains = 0, DoesNotContain, Equals, NotEquals, GreaterThan, LessThan, GreaterThanOrEqual, LessThanOrEqual, StartsWith, EndsWith
};

enum class SmartMatchMode { MatchAll = 0, MatchAny };

struct SmartPlaylistRule {
    SmartRuleField field = SmartRuleField::Title;
    SmartRuleOperator op = SmartRuleOperator::Contains;
    std::string value;
    bool Matches(const MediaItem& item, bool existsOnDisk = true) const;
    bool Validate(std::string* error = nullptr) const;
    // Lazily normalized pattern for Title/Path rules. Rules are matched against
    // every library item per evaluation, so the constant pattern is normalized
    // once per rule instead of once per item.
    const std::wstring& GetLowerPattern() const;
    JsonValue ToJson() const;
    static SmartPlaylistRule FromJson(const JsonValue& json);
    std::string ToString() const;
    static SmartPlaylistRule FromString(const std::string& str);
    // Kept public so the rule stays an aggregate (tests brace-initialize it).
    // Only GetLowerPattern() should read or write these.
    mutable std::string lowerPatternSource;
    mutable std::wstring lowerPatternCache;
    mutable std::string numericSource;
    mutable int numericField = -1;
    mutable long double numericValue = 0;
    mutable bool numericValid = false, relativeDate = false;
};

struct SmartPlaylistCriteria {
    SmartMatchMode matchMode = SmartMatchMode::MatchAll;
    std::vector<SmartPlaylistRule> rules;
    int sortField = 0; // 0: Title, 1: DateAdded, 2: LastPlayed, 3: Duration, 4: Progress
    int sortDirection = 0; // 0: Ascending, 1: Descending
    size_t limit = 0; // 0 = unlimited
    bool Matches(const MediaItem& item, bool existsOnDisk = true) const;
    bool Validate(std::string* error = nullptr) const;
    bool RequiresAvailability() const;
    std::string validationError;
    // Preserve invalid loaded input so an unrelated save cannot normalize it
    // into an enabled rule. Explicit editor repair creates new criteria.
    JsonValue invalidSource;
    bool hasInvalidSource = false;
    JsonValue ToJson() const;
    static SmartPlaylistCriteria FromJson(const JsonValue& json);
    std::string ToRuleString() const;
    static SmartPlaylistCriteria FromRuleString(const std::string& str);
};

enum class PlaylistType { Manual = 0, Smart };

struct Playlist {
    std::string id, name;
    PlaylistType type = PlaylistType::Manual;
    std::vector<std::wstring> itemPaths;
    SmartPlaylistCriteria smartCriteria;
    int64_t createdTimestamp = 0;
    JsonValue extraFields;
    std::string criteriaValidationError;
    bool IsSmart() const { return type == PlaylistType::Smart; }
};

struct HistoryRecord {
    std::wstring filePath, title;
    double lastPosSec = 0.0, durationSec = 0.0;
    int64_t watchedTimestamp = 0;
    JsonValue extraFields;
};

struct PlaylistStats {
    uint64_t totalSizeBytes = 0;
    double totalDurationSec = 0.0;
    int watchedCount = 0, totalCount = 0;
    size_t resumeIndex = 0;
};
enum class ImportConflictPolicy {
    KeepExisting = 0, // Default: preserve existing record, update size/duration if changed/missing
    Overwrite,        // Overwrite existing item title, metadata, timestamps from disk probe
    Skip,             // Skip importing if item already exists
    CreateCopy        // Create duplicate item with a new unique ID
};

struct ImportConflictRecord {
    std::wstring path;
    std::string existingId;
    std::string resolution; // "kept_existing", "overwritten", "skipped", "created_copy"
};

struct MediaImportResult {
    size_t inputCount = 0, discoveredFiles = 0, acceptedFiles = 0, added = 0;
    size_t updated = 0, unchanged = 0, skippedUnsupported = 0, skippedMissing = 0;
    size_t skippedDuplicate = 0, skippedReparse = 0, probeFailures = 0, conflictCount = 0;
    size_t reusedMetadata = 0;
    size_t conflictResolvedKeep = 0, conflictResolvedOverwrite = 0, conflictResolvedSkip = 0, conflictResolvedCopy = 0;
    LongOperationOutcome outcome = LongOperationOutcome::Completed;
    StorageStatus status = StorageStatus::Ok();
    std::vector<ImportConflictRecord> conflicts;
    // Accepted media in deterministic discovery order. This is deliberately returned separately from counters so App can route the exact
    // accepted set into a playlist or playback queue without re-scanning directories.
    std::vector<std::wstring> acceptedPaths;
};

using MediaImportProgressCallback = std::function<bool(size_t processed, size_t total, const std::wstring& currentPath)>;
using MediaProbeFunction = std::function<bool(const std::wstring& path, double& outDurationSec, uint32_t& outWidth, uint32_t& outHeight)>;

struct MetadataBackfillOptions {
    size_t maxItems = 0;                      // 0 = unlimited
    bool forceRetry = false;                  // If true, ignore attempt limits and backoff intervals
    uint32_t maxAttempts = 5;                 // Max failed attempts before item is skipped (0 = unlimited)
    int64_t baseBackoffSec = 30;              // Base backoff for exponential delay
    int64_t maxBackoffSec = 3600;             // Maximum backoff delay cap
    int64_t nowTimestamp = 0;                 // Current timestamp override (0 = system clock)
    MediaProbeFunction probeFunction = {};    // Custom probe override (defaults to ProbeMediaMetadata)
};

struct MetadataBackfillResult {
    size_t totalEligible = 0;                 // Total items in library needing backfill
    size_t processedCount = 0;                // Items attempted in this execution
    size_t completedCount = 0;                // Items successfully probed and updated
    size_t failedCount = 0;                   // Items where probe failed in this execution
    size_t skippedBackoffCount = 0;           // Items skipped due to backoff/max attempts
    LongOperationOutcome outcome = LongOperationOutcome::Completed;
    StorageStatus status = StorageStatus::Ok();
};

// File/probe state used to reject stale worker results. Playback, bookmarks
// and overrides are deliberately absent: they remain owned by the live item.
struct MetadataBackfillBaseline {
    std::string id;
    std::wstring path;
    double durationSec = 0;
    uint64_t size = 0, fileIndex = 0;
    uint32_t volume = 0, width = 0, height = 0, attempts = 0;
    int64_t lastAttempt = 0;
    uint64_t fileWriteTime = 0;
    bool failed = false;
    static bool SameDuration(double a, double b) { return a == b || (std::isnan(a) && std::isnan(b)); }
    static MetadataBackfillBaseline Capture(const MediaItem& item) {
        return {item.id, item.filePath, item.durationSec, item.fileSizeBytes, item.fileIndex, item.volumeSerialNumber,
            item.videoWidth, item.videoHeight, item.probeAttemptCount, item.lastProbeAttemptTimestamp, item.metadataFileWriteTime, item.metadataProbeFailed};
    }
    bool Matches(const MediaItem& item) const {
        return id == item.id && NormalizePathKey(path) == NormalizePathKey(item.filePath) &&
            SameDuration(durationSec, item.durationSec) && size == item.fileSizeBytes && fileIndex == item.fileIndex &&
            volume == item.volumeSerialNumber && width == item.videoWidth && height == item.videoHeight &&
            attempts == item.probeAttemptCount && lastAttempt == item.lastProbeAttemptTimestamp &&
            fileWriteTime == item.metadataFileWriteTime && failed == item.metadataProbeFailed;
    }
};

struct MetadataBackfillMergeResult {
    size_t changedCount = 0, discardedCount = 0;
    bool viewChanged = false;
};

struct MediaImportMergeResult {
    size_t addedCount = 0, updatedCount = 0, discardedCount = 0;
    bool viewChanged = false;
};

using MetadataBackfillProgressCallback = std::function<bool(size_t processed, size_t total, const std::wstring& currentPath)>;
struct MediaLibraryLoadResult {
    bool success = true;
    StorageStatus libraryStatus = StorageStatus::Ok();
    StorageStatus playlistsStatus = StorageStatus::Ok();
    StorageStatus historyStatus = StorageStatus::Ok();
    StorageStatus watchedFoldersStatus = StorageStatus::Ok();
    bool libraryRecoveredFromBackup = false, playlistsRecoveredFromBackup = false, historyRecoveredFromBackup = false;
    bool watchedFoldersRecoveredFromBackup = false;
    bool is_ok() const {
        return success && libraryStatus.is_ok() && playlistsStatus.is_ok() && historyStatus.is_ok() && watchedFoldersStatus.is_ok();
    }
    explicit operator bool() const { return is_ok(); }
};

struct MediaLibrarySaveResult {
    bool success = true;
    StorageStatus libraryStatus = StorageStatus::Ok();
    StorageStatus playlistsStatus = StorageStatus::Ok();
    StorageStatus historyStatus = StorageStatus::Ok();
    StorageStatus watchedFoldersStatus = StorageStatus::Ok();
    bool librarySaved = false, playlistsSaved = false, historySaved = false, watchedFoldersSaved = false;
    bool is_ok() const {
        return success && libraryStatus.is_ok() && playlistsStatus.is_ok() && historyStatus.is_ok() && watchedFoldersStatus.is_ok();
    }
    explicit operator bool() const { return is_ok(); }
};

struct WatchReconcileResult {
    size_t addedCount = 0, updatedCount = 0, removedCount = 0, renamedCount = 0, rescanCount = 0;
    StorageStatus status = StorageStatus::Ok();
    bool needsRescan = false;
};

struct PreparedWatchDelta;

struct MediaRelocationResult {
    size_t missingCount = 0, relocatedCount = 0, ambiguousCount = 0, notFoundCount = 0;
    std::vector<std::pair<std::wstring, std::wstring>> relocatedPaths;
};

struct FolderRediscoveryResult {
    size_t renamedFoldersCount = 0, mergedItemsCount = 0, relocatedItemsCount = 0, removedGhostCount = 0;
    std::vector<std::pair<std::wstring, std::wstring>> renamedFolders; // oldDir, newDir
    StorageStatus status = StorageStatus::Ok();
    bool needsRescan = false;
};

struct PreparedFolderRediscovery;

using LibraryLoadMilestoneCallback = std::function<void(const wchar_t* phase)>;

// Published values never refer to mutable library data. Unchanged media records
// are shared across generations; a progress update copies one record and the
// small pointer vector, rather than every owning title/path/preferences value.
struct MediaLibrarySaveSnapshot {
    using MediaRecords = std::vector<std::shared_ptr<const MediaItem>>;
    std::wstring rootDir;
    uint64_t seriesId = 0, libraryGeneration = 0, playlistsGeneration = 0;
    uint64_t historyGeneration = 0, watchedFoldersGeneration = 0;
    std::shared_ptr<const MediaRecords> mediaItems;
    std::shared_ptr<const std::vector<Playlist>> playlists;
    std::shared_ptr<const std::vector<HistoryRecord>> history;
    std::shared_ptr<const std::vector<std::wstring>> watchedFolders;
    MediaLibraryLoadResult loadResult;
};

// Exclusively owned by the synchronous caller or the serialized save worker.
struct MediaLibrarySaveState {
    uint64_t seriesId = 0, libraryGeneration = 0, playlistsGeneration = 0;
    uint64_t historyGeneration = 0, watchedFoldersGeneration = 0;
    uint64_t serializedStoreCount = 0;
    uint64_t serializedMediaRecordCount = 0, reusedMediaRecordCount = 0;
    bool incrementalLibrary = false, forceLibraryCheckpoint = false, hasLibraryChanges = false;
    uint64_t incrementalWriteCount = 0, libraryCheckpointCount = 0, writtenLibraryBytes = 0;
    BY_HANDLE_FILE_INFORMATION checkpointInfo{};
    std::string checkpointDigest;
    std::map<size_t, std::string> mediaRecordOverrides;
    std::string lastLibraryJson, lastPlaylistsJson, lastHistoryJson, lastWatchedFoldersJson;
    // The full checkpoint supplies unchanged record bytes; bounded overrides
    // supply the latest changed slots. Owning records prevents pointer ABA reuse.
    struct RecordSpan { size_t offset = 0, length = 0; };
    std::shared_ptr<const MediaLibrarySaveSnapshot::MediaRecords> lastMediaRecords;
    std::vector<RecordSpan> lastMediaSpans;
};

MediaLibrarySaveResult WriteLibrarySaveSnapshot(const MediaLibrarySaveSnapshot& snapshot,
                                               MediaLibrarySaveState& state);

// Derived data depends on specific parts of the library. The broad version
// remains the persistence/conflict token; these stamps keep unrelated edits
// from throwing away media order, search text, or history grouping.
struct LibraryRevisions {
    uint64_t catalog = 0, playback = 0, playlists = 0, history = 0;
    bool SamePlaylistData(const LibraryRevisions& other) const {
        return catalog == other.catalog && playback == other.playback && playlists == other.playlists;
    }
    bool SameSearchData(const LibraryRevisions& other) const {
        return catalog == other.catalog && playback == other.playback && history == other.history;
    }
};

class MediaLibrary {
public:
    static constexpr int kCurrentLibrarySchemaVersion = 2, kCurrentPlaylistsSchemaVersion = 2, kCurrentHistorySchemaVersion = 2;
    static constexpr int kCurrentWatchedFoldersSchemaVersion = 1;
    static bool MigrateLibrarySchema(JsonValue& root, int fromVersion, int targetVersion = kCurrentLibrarySchemaVersion);
    static bool MigratePlaylistsSchema(JsonValue& root, int fromVersion, int targetVersion = kCurrentPlaylistsSchemaVersion);
    static bool MigrateHistorySchema(JsonValue& root, int fromVersion, int targetVersion = kCurrentHistorySchemaVersion);
    static bool MigrateWatchedFoldersSchema(JsonValue& root, int fromVersion, int targetVersion = kCurrentWatchedFoldersSchemaVersion);
    std::vector<MediaItem> mediaItems;
    std::vector<Playlist> playlists;
    std::vector<HistoryRecord> history;
    std::vector<std::wstring> watchedFolders;
    MediaLibrary();
    StorageStatus Load();
    // Builds a complete library against an explicit portable root. The
    // overload is used by the startup snapshot worker and deterministic tests;
    // it performs no writes. Relative media paths continue to resolve against
    // the supplied application root, not the process working directory.
    StorageStatus LoadFromRoot(const std::wstring& rootDir, const LibraryLoadMilestoneCallback& milestone = {});
    MediaLibrarySaveResult Save();
    // Copies/moves never inherit the originating model's UI save dispatch.
    void SetSaveRequestHandler(std::function<MediaLibrarySaveResult()> handler) {
        saveRequestOwner = this; saveRequestThread = std::this_thread::get_id(); saveRequestHandler = std::move(handler);
    }
    // Capture on the model's owning thread. Comparison detects direct public
    // edits too; dirty detection must not depend on callers remembering Notify.
    std::shared_ptr<const MediaLibrarySaveSnapshot> CaptureSaveSnapshot(
        const std::shared_ptr<const MediaLibrarySaveSnapshot>& previous = {}) const;
    MediaLibraryLoadResult GetLastLoadResult() const { return lastLoadResult; }
    // Playback can defer probing a new record to the existing metadata worker.
    MediaItem* AddOrUpdateMedia(const std::wstring& path, bool probeMetadata = true);
    // Returns the WebView/mpv-compatible extension set (case-insensitive).
    static const std::set<std::wstring>& GetSupportedExtensions();
    static bool IsSupportedMediaExtension(const std::wstring& path);
    // Imports files and recursively scans directories. The library is saved at most once, after all accepted paths have been processed.
    MediaImportResult ImportPaths(const std::vector<std::wstring>& paths, const MediaImportProgressCallback& progress = {},
                                  bool saveChanges = true, ImportConflictPolicy conflictPolicy = ImportConflictPolicy::KeepExisting,
                                  const MediaImportProbeCache* probeCache = nullptr, const MediaProbeFunction& probeFunction = {});
    MediaImportMergeResult MergeImportedMedia(std::vector<MediaItem> items,
        const std::vector<std::optional<MetadataBackfillBaseline>>& baseline);
    struct RemovedMediaInfo { std::wstring filePath; double durationSec = 0.0; };
    bool RemoveMedia(const std::string& id);
    // Optional owned metadata for cleanup after vector erasure invalidates pointers.
    size_t RemoveMedia(const std::vector<std::string>& ids, size_t* outSkipped = nullptr, bool saveChanges = true,
                       std::vector<RemovedMediaInfo>* outRemoved = nullptr);
    MediaItem* FindByPath(const std::wstring& path);
    const MediaItem* FindByPath(const std::wstring& path) const;
    MediaItem* FindById(const std::string& id);
    const MediaItem* FindById(const std::string& id) const;
    static std::wstring NormalizePathKey(const std::wstring& path);
    void RebuildPathIndex();
    // Moves the most recently watched item to the front while repairing only
    // the two affected index entries. Returns true when an item moved.
    bool PromoteMostRecentlyWatched();
    PlaylistStats GetPlaylistStats(const std::string& playlistId) const;
    void InvalidatePlaylistStatsCache(bool clearSmartPaths = true) const;
    // Conservative notification for legacy callers editing public records.
    // Membership/path edits must also repair lookup maps with RebuildPathIndex.
    void NotifyPlaylistChanged();
    // For edits known to affect only playlists. The legacy notification above
    // remains a conservative invalidation for direct public-record mutations.
    void NotifyPlaylistEdited();
    const LibraryRevisions& GetRevisions() const { return revisions; }
    uint64_t GetVersion() const { return libraryVersion; }
    uint64_t GetSearchContentVersion() const { return searchContentVersion; }
    MediaRelocationResult RelocateMissingMediaToRoot(const std::wstring& searchRoot);
    FolderRediscoveryResult RediscoverFolderChanges(bool saveChanges = true);
    FolderRediscoveryResult ApplyFolderRediscovery(const PreparedFolderRediscovery& prepared, bool saveChanges = true);
    size_t RemoveFolder(const std::wstring& folderPath, bool saveChanges = true);
    size_t RemoveFolders(const std::vector<std::wstring>& folderPaths, bool saveChanges = true);
    const MediaPlaybackPreferences* GetMediaPreferences(const std::wstring& path) const;
    bool SetMediaPreferences(const std::wstring& path, const MediaPlaybackPreferences& prefs);
    const std::vector<MediaBookmark>* GetBookmarks(const std::wstring& path) const;
    bool AddBookmark(const std::wstring& path, const MediaBookmark& bookmark);
    bool RemoveBookmark(const std::wstring& path, const std::string& bookmarkId);
    bool ClearBookmarks(const std::wstring& path);
    const MediaMetadataOverrides* GetMetadataOverrides(const std::wstring& path) const;
    bool SetMetadataOverrides(const std::wstring& path, const MediaMetadataOverrides& overrides);
    bool ClearMetadataOverrides(const std::wstring& path);
    bool RefreshMediaMetadata(const std::wstring& path, bool saveChanges = true, const MediaProbeFunction& probeFn = {});
    void UpdateProgress(const std::wstring& path, double currentPosSec, double totalDurationSec);
    void SetWatched(const std::wstring& path, bool watched);
    size_t SetWatched(const std::vector<std::wstring>& paths, bool watched, size_t* outSkipped = nullptr);
    void AddToHistory(const std::wstring& path, double lastPosSec, double durationSec);
    bool RemoveFromHistory(size_t index);
    void ClearHistory();
    const std::vector<std::wstring>& GetWatchedFolders() const { return watchedFolders; }
    bool AddWatchedFolder(const std::wstring& path, bool saveChanges = true);
    bool RemoveWatchedFolder(const std::wstring& path, bool saveChanges = true);
    WatchReconcileResult ReconcileWatchBatch(const WatchChangeBatch& batch, bool saveChanges = true, bool probeMetadata = true);
    WatchReconcileResult ReconcileRescan(const std::wstring& rootFolder, bool saveChanges = true, bool probeMetadata = true);
    // Apply worker-observed file changes in one transaction, without filesystem
    // calls. Baseline checks protect edits made while the worker was running.
    WatchReconcileResult ApplyPreparedWatchDelta(const PreparedWatchDelta& delta, bool saveChanges = true);
    Playlist* CreatePlaylist(const std::string& name);
    bool DeletePlaylist(const std::string& playlistId);
    bool AddToPlaylist(const std::string& playlistId, const std::wstring& path);
    size_t AddToPlaylist(const std::string& playlistId, const std::vector<std::wstring>& paths, size_t* outSkipped = nullptr);
    bool RemoveFromPlaylist(const std::string& playlistId, size_t itemIndex);
    Playlist* FindPlaylist(const std::string& playlistId);
    const Playlist* FindPlaylist(const std::string& playlistId) const;
    size_t GetPlaylistResumeIndex(const Playlist* playlist) const;
    // Resume scan over an already-materialized path list, so callers holding a
    // GetPlaylistPathsRef() result don't re-materialize it.
    size_t GetPlaylistResumeIndex(const std::vector<std::wstring>& paths) const;
    Playlist* CreateSmartPlaylist(const std::string& name, const SmartPlaylistCriteria& criteria);
    bool UpdateSmartPlaylist(const std::string& playlistId, const std::string& name, const SmartPlaylistCriteria& criteria);
    std::vector<std::wstring> GetPlaylistPaths(const Playlist& playlist) const;
    std::vector<std::wstring> GetPlaylistPaths(const std::string& playlistId) const;
    // Borrowing variant for read-only frame work: manual playlists return a
    // reference to their stored paths; smart playlists resolve through a
    // version-keyed cache so per-frame callers don't re-scan the library.
    const std::vector<std::wstring>& GetPlaylistPathsRef(const Playlist& playlist) const;
    struct PlaylistPathsView { const std::vector<std::wstring>& paths; uint64_t generation; };
    // Resolve before caching row indices: disk availability can change the
    // smart-playlist membership without changing any library revision.
    PlaylistPathsView ResolvePlaylistPaths(const Playlist& playlist) const;
    std::vector<std::wstring> EvaluateSmartPlaylist(const SmartPlaylistCriteria& criteria) const;
    using AvailabilityProvider = std::function<PathAvailabilityObservation(const std::wstring&)>;
    void SetAvailabilityProvider(AvailabilityProvider provider) {
        availabilityProvider = std::move(provider);
        availabilityProviderOwner = this; availabilityProviderThread = std::this_thread::get_id();
        InvalidatePlaylistStatsCache();
    }
    void InvalidateAvailabilityViews() const;
    uint64_t GetSmartPlaylistEvaluationCount() const { return smartPlaylistEvaluationCount; }
    uint64_t GetSmartPlaylistDiskProbeCount() const { return smartPlaylistDiskProbeCount; }
    // Fast metadata extraction using Media Foundation Source Reader
    static bool ProbeMediaMetadata(const std::wstring& path, double& outDurationSec, uint32_t& outWidth, uint32_t& outHeight);
    // Retries missing/failed metadata probes with deterministic backoff and bounding
    MetadataBackfillResult BackfillMissingMetadata(const MetadataBackfillOptions& options = {},
                                                    const MetadataBackfillProgressCallback& progress = {}, bool saveChanges = true);
    MetadataBackfillMergeResult MergeMetadataBackfill(const std::vector<MetadataBackfillBaseline>& baseline,
        const std::vector<MediaItem>& updates);
    static bool NeedsMetadataBackfill(const MediaItem& item);
    static bool CanAttemptMetadataBackfill(const MediaItem& item, const MetadataBackfillOptions& options, int64_t now);
private:
    std::string GenerateId(const std::wstring& path);
    MediaItem* AddOrUpdateMediaCore(const std::wstring& path, bool& changed, bool& metadataProbeFailed, bool probeMetadata = true);
    bool SetWatchedCore(const std::wstring& path, bool watched);
    const MediaLibrary* saveRequestOwner = nullptr;
    std::thread::id saveRequestThread;
    std::function<MediaLibrarySaveResult()> saveRequestHandler;
    AvailabilityProvider availabilityProvider;
    const MediaLibrary* availabilityProviderOwner = nullptr;
    std::thread::id availabilityProviderThread;
    std::wstring portableRoot;
    std::shared_ptr<const MediaLibrarySaveSnapshot> lastSaveSnapshot;
    MediaLibrarySaveState saveState;
    mutable std::unordered_map<std::wstring, size_t> pathToIndex;
    mutable std::unordered_map<std::string, size_t> idToIndex;
    struct PlaylistStatsCacheEntry {
        LibraryRevisions revisions;
        PlaylistStats stats;
        ULONGLONG expiresAtTick = (std::numeric_limits<ULONGLONG>::max)();
        int64_t evaluatedAtSec = 0, expiresAtSec = (std::numeric_limits<int64_t>::max)();
        bool timeDependent = false;
    };
    mutable std::unordered_map<std::string, PlaylistStatsCacheEntry> playlistStatsCache;
    struct SmartPathsCacheEntry {
        LibraryRevisions revisions;
        uint64_t generation = 0;
        ULONGLONG expiresAtTick = (std::numeric_limits<ULONGLONG>::max)();
        int64_t evaluatedAtSec = 0, expiresAtSec = (std::numeric_limits<int64_t>::max)();
        bool timeDependent = false, playbackDependent = false, availabilityDependent = false;
        std::vector<std::wstring> paths;
    };
    mutable std::unordered_map<std::string, SmartPathsCacheEntry> smartPathsCache;
    struct SmartAvailabilityEntry { PathAvailability value = PathAvailability::Unavailable; ULONGLONG checkedAtTick = 0; bool known = false; };
    mutable std::vector<SmartAvailabilityEntry> smartAvailability;
    mutable uint64_t smartAvailabilityCatalog = (std::numeric_limits<uint64_t>::max)();
    std::vector<std::wstring> EvaluatePreparedSmartPlaylist(const PreparedSmartPlaylist& plan,
        ULONGLONG& expiresAtTick, int64_t& expiresAtSec, bool useAvailabilityCache = true) const;
    mutable uint64_t smartPathsGeneration = 0;
    mutable uint64_t smartPlaylistEvaluationCount = 0, smartPlaylistDiskProbeCount = 0;
    uint64_t libraryVersion = 0;
    LibraryRevisions revisions;
    uint64_t searchContentVersion = 0;
    StorageStatus lastLoadStatus = StorageStatus::Ok();
    MediaLibraryLoadResult lastLoadResult;
    MediaLibrarySaveResult lastSaveResult;
};

} // namespace Aperture
