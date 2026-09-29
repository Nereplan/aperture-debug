#pragma once

#include "Common.h"
#include "MediaLibrary.h"
#include "TextInputModel.h"
#include "Accessibility.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <limits>
#include <utility>

namespace Aperture {

// Source origin of a search result item
enum class GlobalSearchResultSource {
    Media,           // Present in media library
    History,         // Present in playback history only
    MediaAndHistory  // Present in both media library and history
};

// Action produced by activating a search result
enum class GlobalSearchActionType { None = 0, PlayMedia, OpenInExplorer, ReportMissingMedia };

// Result payload of an executed search surface action
struct GlobalSearchActionResult {
    GlobalSearchActionType action = GlobalSearchActionType::None;
    std::wstring filePath;
    double startPosSec = 0.0;
    bool isMissing = false;
    std::wstring message;
};

// Individual projected search result item
struct GlobalSearchResultItem {
    std::wstring filePath, title, fileName;
    double durationSec = 0.0, lastPosSec = 0.0;
    int64_t lastWatchedTimestamp = 0, addedTimestamp = 0;
    GlobalSearchResultSource source = GlobalSearchResultSource::Media;
    bool existsOnDisk = true;
    int matchScore = 0;
    double GetProgressRatio() const { if (durationSec <= 0.0) return 0.0; return std::clamp(lastPosSec / durationSec, 0.0, 1.0); }
    std::wstring GetFormattedDuration() const;
    std::wstring GetSourceDisplayName() const;
};

// Query configuration options
struct GlobalSearchQueryOptions { size_t maxResults = 50; bool checkFileExists = true, includeMedia = true, includeHistory = true; };

// Immutable snapshot of global search state for rendering / testing
struct GlobalSearchProjection {
    std::wstring query;
    std::vector<GlobalSearchResultItem> results;
    size_t totalMatchCount = 0;
    int selectedIndex = -1;
    bool isDebouncing = false, isOpen = false;
    bool HasSelection() const { return selectedIndex >= 0 && selectedIndex < static_cast<int>(results.size()); }
    const GlobalSearchResultItem* GetSelectedItem() const {
        if (HasSelection()) { return &results[static_cast<size_t>(selectedIndex)]; }
        return nullptr;
    }
};

// Cached entry inside GlobalSearchIndex with pre-computed normalized keys
struct CachedSearchEntry {
    std::wstring filePath, title, fileName, normalizedTitle;
    std::wstring normalizedFileName, normalizedPath;
    std::vector<std::wstring> titleTokens;
    std::vector<std::wstring> pathTokens;
    double durationSec = 0.0, lastPosSec = 0.0;
    int64_t lastWatchedTimestamp = 0, addedTimestamp = 0;
    GlobalSearchResultSource source = GlobalSearchResultSource::Media;
};

// In-memory normalized index derived from MediaLibrary without external stores
class GlobalSearchIndex {
public:
    GlobalSearchIndex() = default;
    // Normalization utilities
    static std::wstring NormalizeText(const std::wstring& text);
    static std::vector<std::wstring> Tokenize(const std::wstring& text);
    static std::wstring ExtractFileName(const std::wstring& path);
    static bool CheckPathExistsOnDisk(const std::wstring& path);
    // Cache management
    bool IsDirty(const MediaLibrary& library) const;
    void Invalidate();
    void RebuildIndex(const MediaLibrary& library);
    size_t GetEntryCount() const { return entries.size(); }
    // Search query execution
    std::vector<GlobalSearchResultItem> Query(const MediaLibrary& library, const std::wstring& rawQuery,
                                             const GlobalSearchQueryOptions& options, size_t& outTotalMatchCount);
    std::vector<GlobalSearchResultItem> Query(const MediaLibrary& library, const std::wstring& rawQuery,
                                             const GlobalSearchQueryOptions& options = {}) {
        size_t total = 0;
        return Query(library, rawQuery, options, total);
    }
    // Relevance scoring
    static int CalculateRelevanceScore(const CachedSearchEntry& entry, const std::wstring& normalizedQuery,
                                       const std::vector<std::wstring>& queryTokens);

private:
    std::vector<CachedSearchEntry> entries;
    uint64_t cachedLibraryVersion = (std::numeric_limits<uint64_t>::max)();
};

// Global search state model with input editing, debounce, keyboard navigation, and actions
class GlobalSearchModel {
public:
    explicit GlobalSearchModel(uint32_t debounceDelayMs = 150);
    // Visibility / Open state
    void Open();
    void Close();
    void Toggle();
    bool IsOpen() const { return isOpen; }
    // Query & Text Input
    void SetQuery(const std::wstring& query, const MediaLibrary& library, bool immediate = false);
    const std::wstring& GetQuery() const { return currentQuery; }
    TextInputModel& GetTextInputModel() { return textInput; }
    const TextInputModel& GetTextInputModel() const { return textInput; }
    void SetQueryOptions(const GlobalSearchQueryOptions& options) { queryOptions = options; }
    // Timing & Debounce processing Returns true if a debounced search completed and projection was updated
    bool Tick(uint32_t elapsedMs, const MediaLibrary& library);
    void ExecuteQueryNow(const MediaLibrary& library);
    // Keyboard navigation
    void MoveSelectionDown(bool wrap = false);
    void MoveSelectionUp(bool wrap = false);
    void MoveSelectionPageDown(size_t pageSize = 5);
    void MoveSelectionPageUp(size_t pageSize = 5);
    void MoveSelectionHome();
    void MoveSelectionEnd();
    void SetSelectedIndex(int index);
    int GetSelectedIndex() const { return selectedIndex; }
    void ClearSelection();
    // Action execution on selection
    GlobalSearchActionResult ActivateSelection(const MediaLibrary& library);
    GlobalSearchActionResult OpenExplorerForSelection(const MediaLibrary& library);
    // Projection snapshot
    GlobalSearchProjection GetProjection() const;
    // Borrowed results remain valid until the next query executes.
    const std::vector<GlobalSearchResultItem>& GetResults() const { return currentResults; }
    size_t GetTotalMatchCount() const { return currentTotalMatches; }
    GlobalSearchIndex& GetIndex() { return index; }
    const GlobalSearchIndex& GetIndex() const { return index; }

private:
    GlobalSearchIndex index;
    TextInputModel textInput;
    GlobalSearchQueryOptions queryOptions;
    std::wstring currentQuery;
    std::vector<GlobalSearchResultItem> currentResults;
    size_t currentTotalMatches = 0;
    int selectedIndex = -1;
    uint32_t debounceDelayMs = 150, pendingDebounceElapsedMs = 0;
    bool pendingDebounce = false, isOpen = false;
    void ClampSelection();
    void ScheduleQuery(const MediaLibrary& library, bool immediate);
    GlobalSearchActionResult BuildSelectionAction(GlobalSearchActionType action) const;
};

// Converts std::u16string to std::wstring helper
inline std::wstring Text16ToWideString(const TextInputModel::Text& value) {
    std::wstring result;
    result.reserve(value.size());
    for (char16_t c : value) result.push_back(static_cast<wchar_t>(c));
    return result;
}

// Converts std::wstring to std::u16string helper
inline TextInputModel::Text WideStringToText16(const std::wstring& value) {
    TextInputModel::Text result;
    result.reserve(value.size());
    for (wchar_t c : value) result.push_back(static_cast<char16_t>(c));
    return result;
}

} // namespace Aperture
