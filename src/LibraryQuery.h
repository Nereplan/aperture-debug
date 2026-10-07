#pragma once

#include "Common.h"
#include "MediaLibrary.h"
#include "Settings.h"

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <cstdint>
#include <algorithm>
#include <functional>

namespace Aperture {

// ============================================================================
// 1. Library Sort & Filter Enumerations
// ============================================================================

enum class LibrarySortField { Title = 0, DateAdded, LastPlayed, Duration, Progress, Manual };

enum class SortDirection { Ascending = 0, Descending };

enum class LibraryStatusFilter { All = 0, Unwatched, InProgress, Completed, Missing };

enum class LibraryGroupMode { Flat = 0, Folder, Series, Duplicates };

enum class DuplicateMatchKind {
    ExactFileIdentity = 0, // Exact Windows file identity (volumeSerialNumber + fileIndex)
    ExactPathAlias,        // Normalized path alias (case, separators, relative/canonical representation)
    LikelyContent          // Conservative metadata & size evidence (file size, duration, video dimensions, title stem)
};

struct DuplicateGroup {
    std::wstring key;
    DuplicateMatchKind matchKind = DuplicateMatchKind::LikelyContent;
    std::wstring title, subtitle;
    std::vector<const MediaItem*> items;
    std::vector<size_t> originalIndices;
    uint64_t individualSizeBytes = 0, totalDuplicateSizeBytes = 0;
    double durationSec = 0.0;
    size_t suggestedKeepIndex = 0;
};

struct DuplicateDetectionResult {
    std::vector<DuplicateGroup> groups;
    size_t totalDuplicateEntries = 0, totalDuplicateGroups = 0;
    uint64_t totalReclaimableBytes = 0;
};

// ============================================================================
// 2. Query Configuration & Projection Models
// ============================================================================

struct LibraryGroupSummary {
    std::wstring key;              // Deterministic group key (e.g. normalized directory or series stem)
    std::wstring title;            // Group header display title (e.g. folder name or series title)
    std::wstring subtitle;         // Subtitle (e.g. directory path or episode count)
    size_t startIndex = 0;         // Starting index into the projection rows
    size_t count = 0;              // Number of items in this group
    uint64_t totalSizeBytes = 0;
    double totalDurationSec = 0.0;
    bool isSpecial = false;        // E.g. "Other Videos" / "Standalone"
    bool isCollapsed = false;
};
// ============================================================================

struct LibraryQueryOptions {
    LibrarySortField sortField = LibrarySortField::Title;
    SortDirection sortDirection = SortDirection::Ascending;
    LibraryStatusFilter statusFilter = LibraryStatusFilter::All;
    LibraryGroupMode groupMode = LibraryGroupMode::Flat;
    std::wstring searchQuery = L"";
    size_t maxItems = 0;              // 0 = unlimited, >0 = capped projection
    bool checkFileExists = false;     // Validate disk presence (auto-enabled for Missing filter)
    bool pinLastWatched = false;      // Pin the most recently watched video to top in Flat mode
    // Shared, immutable snapshot of the collapsed-group keys. Callers keep one
    // canonical snapshot and rebuild it only when the key set changes, so
    // building per-frame query options never deep-copies the set.
    std::shared_ptr<const std::unordered_set<std::wstring>> collapsedGroups;
    // Immutable snapshot: never mutate a shared source after submitting it.
    std::shared_ptr<const std::vector<std::wstring>> customFolderOrder;
    const std::vector<std::wstring>& FolderOrder() const {
        static const std::vector<std::wstring> empty;
        return customFolderOrder ? *customFolderOrder : empty;
    }
    bool CollapsedGroupCount(const std::wstring& key) const {
        return collapsedGroups && collapsedGroups->count(key) > 0;
    }
    bool CollapsedGroupsEqual(const decltype(collapsedGroups)& other) const {
        const bool aEmpty = !collapsedGroups || collapsedGroups->empty();
        const bool bEmpty = !other || other->empty();
        if (aEmpty && bEmpty) return true;
        if (aEmpty != bEmpty) return false;
        if (collapsedGroups.get() == other.get()) return true;
        return *collapsedGroups == *other;
    }
    bool operator==(const LibraryQueryOptions& other) const {
        return sortField == other.sortField && sortDirection == other.sortDirection && statusFilter == other.statusFilter &&
               groupMode == other.groupMode && searchQuery == other.searchQuery && maxItems == other.maxItems &&
               checkFileExists == other.checkFileExists && pinLastWatched == other.pinLastWatched &&
               CollapsedGroupsEqual(other.collapsedGroups) && (customFolderOrder == other.customFolderOrder || FolderOrder() == other.FolderOrder());
    }
    bool operator!=(const LibraryQueryOptions& other) const { return !(*this == other); }
};

struct LibraryItemProjection {
    std::string id;
    std::wstring filePath, title;
    double durationSec = 0.0, lastPosSec = 0.0;
    std::string status = "new"; // "new", "in_progress", "done"
    double progressRatio = 0.0;
    int64_t addedTimestamp = 0, lastWatchedTimestamp = 0;
    uint64_t fileSizeBytes = 0;
    bool existsOnDisk = true;
    size_t originalLibraryIndex = 0;
    std::wstring GetFormattedDuration() const;
};

struct LibraryQueryProjection {
    std::vector<LibraryItemProjection> items;
    std::vector<LibraryGroupSummary> groups;
    size_t totalLibraryCount = 0, filteredCount = 0;
    LibraryQueryOptions options;
    uint64_t libraryVersion = 0;
    bool isCapped = false;
    bool IsEmpty() const { return items.empty(); }
    size_t Count() const { return items.size(); }
    size_t GroupCount() const { return groups.size(); }
};

// The UI already owns the media records. Its query result stores only order,
// membership, and group summaries; details are read for visible rows only.
// Indices are valid for the catalog revision that produced this projection.
struct LibraryViewProjection {
    std::vector<size_t> indices;
    // Empty means every projected record is assumed present (no disk probes).
    std::vector<uint8_t> availability;
    std::vector<LibraryGroupSummary> groups;
    size_t totalLibraryCount = 0, filteredCount = 0;
    LibraryQueryOptions options;
    uint64_t libraryVersion = 0, generation = 0;
    bool isCapped = false;
    bool IsEmpty() const { return indices.empty(); }
    size_t Count() const { return indices.size(); }
    size_t GroupCount() const { return groups.size(); }
};

// ============================================================================
// 3. Episode Information & Conservative Next-Episode Models
// ============================================================================

struct EpisodeInfo {
    std::wstring rawPath, rawFileName, directoryPath;
    std::wstring seriesStem;       // Normalized lowercase series name without tokens/brackets
    int seasonNumber = 1;          // 1-based (defaults to 1 if season omitted)
    int episodeNumber = 0;         // 1-based episode number
    bool hasSeason = false;        // True if explicit season token was found
    bool hasEpisode = false;       // True if numeric episode token was found
    bool isValid = false;          // True if recognized as a valid episode sequence item
};

enum class NextEpisodeActionType { None = 0, PlayNextEpisode, NoNextEpisode, Ambiguous, GapDetected, CrossDirectoryRejected, Disabled };

struct NextEpisodeSafeAction {
    NextEpisodeActionType actionType = NextEpisodeActionType::None;
    std::wstring currentFilePath, nextFilePath, nextTitle;
    EpisodeInfo currentEpisode;
    EpisodeInfo nextEpisode;
    bool isAutoPlayAllowed = false, canExecuteExplicitly = false, canExecuteAuto = false;
    std::wstring reason;
};

// ============================================================================
// 4. Library Query & Sequence Engine
// ============================================================================

class LibraryQueryEngine {
public:
    LibraryQueryEngine() = default;
    // Normalization & String Utilities
    static std::wstring NormalizeSearchText(const std::wstring& text);
    static std::wstring ExtractSeriesStem(const std::wstring& filename);
    static EpisodeInfo ParseEpisodeInfo(const std::wstring& filePath);
    static bool AreSameDirectory(const std::wstring& path1, const std::wstring& path2);
    static bool MatchesStatusFilter(const MediaItem& item, LibraryStatusFilter filter, bool existsOnDisk);
    static bool MatchesSearchQuery(const MediaItem& item, const std::wstring& normalizedQuery);
    static int CompareItems(const MediaItem& a, const MediaItem& b, LibrarySortField sortField, SortDirection direction);
    // Query Execution (Direct & Cached) Returns a reference backed by the engine's cache: no projection copy on hit or miss. The reference
    // stays valid until the next Query() or
    // InvalidateCache() on this engine.
    const LibraryQueryProjection& Query(const MediaLibrary& library, const LibraryQueryOptions& options,
                                 const std::function<bool(const std::wstring&)>& fileExistsFn = nullptr);
    // Cache management
    void InvalidateCache();
    bool IsCacheValid(const MediaLibrary& library, const LibraryQueryOptions& options) const;
    // Borrowed until the next Query/QueryView/InvalidateCache/transfer on this
    // engine. Read media details from the matching live library, not this view.
    const LibraryViewProjection& QueryView(const MediaLibrary& library, const LibraryQueryOptions& options,
        const std::function<bool(const std::wstring&)>& fileExistsFn = nullptr);
    uint64_t GetPlanBuildCount() const { return planBuildCount; }
    uint64_t GetViewBuildCount() const { return viewBuildCount; }
    uint64_t GetSearchTextBuildCount() const { return searchTextBuildCount; }
    LibraryViewProjection ReleaseCachedView();
    bool PrimeViewCache(const MediaLibrary& library, const LibraryQueryOptions& options, LibraryViewProjection&& projection);
    // Startup workers can transfer a fully built projection without a deep copy, then prime the UI-owned cache after the matching library
    // moves to the main thread.
    LibraryQueryProjection ReleaseCachedProjection();
    bool PrimeCache(const MediaLibrary& library, const LibraryQueryOptions& options, LibraryQueryProjection&& projection);
    // Conservative Next-Episode Detection
    static std::optional<EpisodeInfo> FindNextEpisode( const std::wstring& currentFilePath, const std::vector<std::wstring>& candidatePaths,
        std::wstring* outAmbiguityDetails = nullptr);
    static std::optional<EpisodeInfo> FindNextEpisode( const std::wstring& currentFilePath, const MediaLibrary& library,
        std::wstring* outAmbiguityDetails = nullptr);
    // Safe Action Projection (follows autoplay settings and ambiguity checks)
    static NextEpisodeSafeAction ProjectNextEpisodeAction( const std::wstring& currentFilePath, const MediaLibrary& library,
        const Settings& settings, const std::function<bool(const std::wstring&)>& fileExistsFn = nullptr);
    static NextEpisodeSafeAction ProjectNextEpisodeAction( const std::wstring& currentFilePath, const std::vector<std::wstring>& candidates,
        const Settings& settings, const std::function<bool(const std::wstring&)>& fileExistsFn = nullptr);
    // Conservative Duplicate Detection
    static DuplicateDetectionResult DetectDuplicates(const MediaLibrary& library);
    static DuplicateDetectionResult DetectDuplicates(const std::vector<MediaItem>& items);

private:
    static LibraryViewProjection BuildView(const MediaLibrary& library, const LibraryQueryOptions& options,
        const std::function<bool(const std::wstring&)>& fileExistsFn);
    LibraryViewProjection FilterPlan(const MediaLibrary& library, const LibraryQueryOptions& options);
    struct SearchFields { std::wstring title, originalTitle, path, series; };
    std::vector<SearchFields> searchFields;
    uint64_t searchCatalogRevision = (std::numeric_limits<uint64_t>::max)();
    LibraryViewProjection cachedView, cachedPlan;
    LibraryRevisions viewRevisions, planRevisions;
    bool hasView = false, hasPlan = false;
    uint64_t viewGeneration = 0, planBuildCount = 0, viewBuildCount = 0, searchTextBuildCount = 0;
    LibraryQueryProjection cachedProjection;
    LibraryRevisions projectionRevisions;
    uint64_t cachedLibraryVersion = (std::numeric_limits<uint64_t>::max)();
};

// UI Display Helpers
std::wstring GetGroupModeDisplayName(LibraryGroupMode mode);
} // namespace Aperture
