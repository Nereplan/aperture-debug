#include "GlobalSearch.h"
#include <cwctype>
#include <sstream>
#include <iomanip>

namespace Aperture {

// ============================================================================
// GlobalSearchResultItem Helpers
// ============================================================================

std::wstring GlobalSearchResultItem::GetFormattedDuration() const {
    if (durationSec <= 0.0) return L"0:00";
    int totalSec = static_cast<int>(durationSec + 0.5);
    int hours = totalSec / 3600;
    int minutes = (totalSec % 3600) / 60;
    int seconds = totalSec % 60;
    wchar_t buf[16];
    if (hours > 0) { swprintf_s(buf, L"%d:%02d:%02d", hours, minutes, seconds); }
    else { swprintf_s(buf, L"%d:%02d", minutes, seconds); }
    return buf;
}

std::wstring GlobalSearchResultItem::GetSourceDisplayName() const {
    switch (source) {
        case GlobalSearchResultSource::Media: return L"Library";
        case GlobalSearchResultSource::History: return L"History";
        case GlobalSearchResultSource::MediaAndHistory: return L"Library & History";
    }
    return L"";
}

// ============================================================================
// GlobalSearchIndex Normalization & Utility
// ============================================================================

std::wstring GlobalSearchIndex::NormalizeText(const std::wstring& text) {
    if (text.empty()) return L"";
    // 1. Trim leading whitespace
    size_t start = 0;
    while (start < text.size() && (std::iswspace(text[start]) || text[start] == L'\0')) { start++; }
    if (start >= text.size()) return L"";
    // 2. Trim trailing whitespace
    size_t end = text.size();
    while (end > start && (std::iswspace(text[end - 1]) || text[end - 1] == L'\0')) { end--; }
    // 3. Lowercase, normalize path separators, and collapse multiple whitespace
    std::wstring result;
    result.reserve(end - start);
    bool lastWasSpace = false;
    for (size_t i = start; i < end; ++i) {
        wchar_t c = text[i];
        if (c == L'/') { c = L'\\'; }
        if (std::iswspace(c)) {
            if (!lastWasSpace) { result.push_back(L' '); lastWasSpace = true; }
        } else { result.push_back(static_cast<wchar_t>(std::towlower(c))); lastWasSpace = false; }
    }
    return result;
}

std::vector<std::wstring> GlobalSearchIndex::Tokenize(const std::wstring& text) {
    std::vector<std::wstring> tokens;
    if (text.empty()) return tokens;
    auto isDelimiter = [](wchar_t c) -> bool {
        if (std::iswspace(c)) return true;
        switch (c) {
            case L'.': case L'_': case L'-': case L'\\': case L'/':
            case L'[': case L']': case L'(': case L')': case L'{': case L'}':
            case L',': case L';': case L':': case L'!': case L'?': case L'+':
            case L'=': case L'\'': case L'"': case L'@': case L'#': case L'$':
            case L'%': case L'^': case L'&': case L'*': case L'|': case L'~':
            case L'`': case L'<': case L'>':
                return true;
            default:
                return false;
        }
    };
    std::wstring current;
    for (wchar_t c : text) {
        if (isDelimiter(c)) {
            if (!current.empty()) { tokens.push_back(current); current.clear(); }
        } else { current.push_back(static_cast<wchar_t>(std::towlower(c))); }
    }
    if (!current.empty()) { tokens.push_back(current); }
    return tokens;
}

std::wstring GlobalSearchIndex::ExtractFileName(const std::wstring& path) {
    if (path.empty()) return L"";
    size_t lastSlash = path.find_last_of(L"\\/");
    if (lastSlash == std::wstring::npos) { return path; }
    return path.substr(lastSlash + 1);
}

bool GlobalSearchIndex::CheckPathExistsOnDisk(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

// ============================================================================
// GlobalSearchIndex Cache Management & Search
// ============================================================================

bool GlobalSearchIndex::IsDirty(const MediaLibrary& library) const { return library.GetVersion() != cachedLibraryVersion; }

void GlobalSearchIndex::Invalidate() { cachedLibraryVersion = (std::numeric_limits<uint64_t>::max)(); entries.clear(); }

void GlobalSearchIndex::RebuildIndex(const MediaLibrary& library) {
    entries.clear();
    std::unordered_map<std::wstring, size_t> pathToEntryIndex;
    const size_t entryCapacity = library.mediaItems.size() + library.history.size();
    entries.reserve(entryCapacity);
    pathToEntryIndex.reserve(entryCapacity);
    // 1. Process Media Items from MediaLibrary
    for (const auto& item : library.mediaItems) {
        std::wstring normKey = MediaLibrary::NormalizePathKey(item.filePath);
        if (normKey.empty()) continue;
        CachedSearchEntry entry;
        entry.filePath = item.filePath;
        entry.title = item.GetDisplayTitle();
        entry.fileName = ExtractFileName(item.filePath);
        entry.normalizedTitle = NormalizeText(entry.title);
        entry.normalizedFileName = NormalizeText(entry.fileName);
        entry.normalizedPath = NormalizeText(item.filePath);
        entry.titleTokens = Tokenize(entry.title);
        entry.pathTokens = Tokenize(item.filePath);
        entry.durationSec = item.durationSec;
        entry.lastPosSec = item.lastPosSec;
        entry.lastWatchedTimestamp = item.lastWatchedTimestamp;
        entry.addedTimestamp = item.addedTimestamp;
        entry.source = GlobalSearchResultSource::Media;
        pathToEntryIndex[normKey] = entries.size();
        entries.push_back(std::move(entry));
    }
    // 2. Process History Records from MediaLibrary
    for (const auto& hist : library.history) {
        std::wstring normKey = MediaLibrary::NormalizePathKey(hist.filePath);
        if (normKey.empty()) continue;
        auto it = pathToEntryIndex.find(normKey);
        if (it != pathToEntryIndex.end()) {
            // Already present in library, elevate source and merge latest stats
            CachedSearchEntry& existing = entries[it->second];
            existing.source = GlobalSearchResultSource::MediaAndHistory;
            if (hist.watchedTimestamp > existing.lastWatchedTimestamp) {
                existing.lastWatchedTimestamp = hist.watchedTimestamp;
                existing.lastPosSec = hist.lastPosSec;
            }
            if (existing.title.empty() && !hist.title.empty()) {
                existing.title = hist.title;
                existing.normalizedTitle = NormalizeText(hist.title);
                existing.titleTokens = Tokenize(hist.title);
            }
            if (existing.durationSec <= 0.0 && hist.durationSec > 0.0) { existing.durationSec = hist.durationSec; }
        } else {
            // History only entry
            CachedSearchEntry entry;
            entry.filePath = hist.filePath;
            entry.title = hist.title.empty() ? ExtractFileName(hist.filePath) : hist.title;
            entry.fileName = ExtractFileName(hist.filePath);
            entry.normalizedTitle = NormalizeText(entry.title);
            entry.normalizedFileName = NormalizeText(entry.fileName);
            entry.normalizedPath = NormalizeText(hist.filePath);
            entry.titleTokens = Tokenize(entry.title);
            entry.pathTokens = Tokenize(hist.filePath);
            entry.durationSec = hist.durationSec;
            entry.lastPosSec = hist.lastPosSec;
            entry.lastWatchedTimestamp = hist.watchedTimestamp;
            entry.addedTimestamp = hist.watchedTimestamp;
            entry.source = GlobalSearchResultSource::History;
            pathToEntryIndex[normKey] = entries.size();
            entries.push_back(std::move(entry));
        }
    }
    cachedLibraryVersion = library.GetVersion();
}

int GlobalSearchIndex::CalculateRelevanceScore(const CachedSearchEntry& entry, const std::wstring& normalizedQuery,
                                               const std::vector<std::wstring>& queryTokens) {
    if (normalizedQuery.empty()) return 0;
    int score = 0;
    // 1. Direct Single-Phrase Checks
    if (entry.normalizedTitle == normalizedQuery) {
        score += 10000;
    } else if (!entry.normalizedTitle.empty() && entry.normalizedTitle.rfind(normalizedQuery, 0) == 0) {
        // Title prefix match
        int lengthBonus = (std::max)(0, 100 - static_cast<int>(entry.normalizedTitle.length()));
        score += 6000 + lengthBonus;
    } else if (entry.normalizedFileName == normalizedQuery) {
        score += 5000;
    } else if (!entry.normalizedFileName.empty() && entry.normalizedFileName.rfind(normalizedQuery, 0) == 0) { score += 4000; } else {
        // Token boundary / prefix matches in title
        bool matchedTokenPrefix = false;
        for (const auto& tok : entry.titleTokens) {
            if (tok.rfind(normalizedQuery, 0) == 0) { score += 3000; matchedTokenPrefix = true; break; }
        }
        if (!matchedTokenPrefix) {
            // Substring in title
            size_t titlePos = entry.normalizedTitle.find(normalizedQuery);
            if (titlePos != std::wstring::npos) {
                int posPenalty = static_cast<int>((std::min)(titlePos, size_t(500)));
                score += 2000 - posPenalty;
            } else {
                // Substring in filename
                size_t filePos = entry.normalizedFileName.find(normalizedQuery);
                if (filePos != std::wstring::npos) {
                    int posPenalty = static_cast<int>((std::min)(filePos, size_t(500)));
                    score += 1500 - posPenalty;
                } else {
                    // Substring in full path
                    size_t pathPos = entry.normalizedPath.find(normalizedQuery);
                    if (pathPos != std::wstring::npos) { score += 800; }
                }
            }
        }
    }
    // 2. Multi-Token Query Refinement
    if (queryTokens.size() > 1) {
        int tokenMatchScore = 0;
        bool allTokensMatched = true;
        for (const auto& qTok : queryTokens) {
            bool tokenFound = false;
            // Check title tokens
            for (const auto& tTok : entry.titleTokens) {
                if (tTok == qTok) {
                    tokenMatchScore += 600;
                    tokenFound = true;
                    break;
                } else if (tTok.rfind(qTok, 0) == 0) { tokenMatchScore += 400; tokenFound = true; break; }
            }
            // Check title substring if not matched in tokens
            if (!tokenFound && entry.normalizedTitle.find(qTok) != std::wstring::npos) { tokenMatchScore += 300; tokenFound = true; }
            // Check path tokens / path substring
            if (!tokenFound) {
                for (const auto& pTok : entry.pathTokens) {
                    if (pTok == qTok || pTok.rfind(qTok, 0) == 0) { tokenMatchScore += 200; tokenFound = true; break; }
                }
            }
            if (!tokenFound && entry.normalizedPath.find(qTok) != std::wstring::npos) { tokenMatchScore += 100; tokenFound = true; }
            if (!tokenFound) { allTokensMatched = false; break; }
        }
        if (allTokensMatched) { score += tokenMatchScore; } else if (score < 5000) {
            // For multi-token queries that didn't exact-match title/filename, all tokens must match
            return 0;
        }
    }
    return score;
}

std::vector<GlobalSearchResultItem> GlobalSearchIndex::Query(const MediaLibrary& library, const std::wstring& rawQuery,
                                                             const GlobalSearchQueryOptions& options, size_t& outTotalMatchCount) {
    outTotalMatchCount = 0;
    std::wstring normalizedQuery = NormalizeText(rawQuery);
    if (normalizedQuery.empty()) { return {}; }
    if (IsDirty(library)) { RebuildIndex(library); }
    std::vector<std::wstring> queryTokens = Tokenize(normalizedQuery);
    struct ScoredMatch { int score = 0; const CachedSearchEntry* entry = nullptr; };
    std::vector<ScoredMatch> matches;
    const size_t capacity = (std::min)(options.maxResults, entries.size());
    // Heap selection pays off for small result windows. Large windows keep
    // the full-sort path, avoiding heap-sort overhead for near-total results.
    const bool bounded = capacity < entries.size() / 4;
    matches.reserve(bounded ? capacity : entries.size());
    // Deterministic Relevance & Recency Ordering:
    // 1. Relevance Score (descending)
    // 2. Last Watched Timestamp (descending)
    // 3. Added Timestamp (descending)
    // 4. Title (case-insensitive lexical ascending)
    // 5. FilePath (case-insensitive lexical ascending for total tie-break stability)
    const auto better = [](const ScoredMatch& a, const ScoredMatch& b) {
        if (a.score != b.score) { return a.score > b.score; }
        if (a.entry->lastWatchedTimestamp != b.entry->lastWatchedTimestamp)
            return a.entry->lastWatchedTimestamp > b.entry->lastWatchedTimestamp;
        if (a.entry->addedTimestamp != b.entry->addedTimestamp) { return a.entry->addedTimestamp > b.entry->addedTimestamp; }
        int titleCmp = _wcsicmp(a.entry->title.c_str(), b.entry->title.c_str());
        if (titleCmp != 0) { return titleCmp < 0; }
        return _wcsicmp(a.entry->filePath.c_str(), b.entry->filePath.c_str()) < 0;
    };
    // Keep only the requested best matches. With this comparator the heap
    // root is the worst retained result, so rejected candidates cost one
    // comparison and no allocation. Count every match independently of the cap.
    for (const auto& entry : entries) {
        if (entry.source == GlobalSearchResultSource::Media && !options.includeMedia) { continue; }
        if (entry.source == GlobalSearchResultSource::History && !options.includeHistory) { continue; }
        const int score = CalculateRelevanceScore(entry, normalizedQuery, queryTokens);
        if (score <= 0) continue;
        ++outTotalMatchCount;
        if (capacity == 0) continue;
        const ScoredMatch candidate{score, &entry};
        if (!bounded) { matches.push_back(candidate); continue; }
        if (matches.size() < capacity) {
            matches.push_back(candidate);
            if (matches.size() == capacity) std::make_heap(matches.begin(), matches.end(), better);
        } else if (better(candidate, matches.front())) {
            std::pop_heap(matches.begin(), matches.end(), better);
            matches.back() = candidate;
            std::push_heap(matches.begin(), matches.end(), better);
        }
    }
    if (bounded && matches.size() == capacity) {
        std::sort_heap(matches.begin(), matches.end(), better);
    } else {
        std::sort(matches.begin(), matches.end(), better);
    }
    const size_t projectionCount = (std::min)(capacity, matches.size());
    std::vector<GlobalSearchResultItem> results;
    results.reserve(projectionCount);
    for (size_t i = 0; i < projectionCount; ++i) {
        const auto* src = matches[i].entry;
        GlobalSearchResultItem item;
        item.filePath = src->filePath;
        item.title = src->title;
        item.fileName = src->fileName;
        item.durationSec = src->durationSec;
        item.lastPosSec = src->lastPosSec;
        item.lastWatchedTimestamp = src->lastWatchedTimestamp;
        item.addedTimestamp = src->addedTimestamp;
        item.source = src->source;
        item.matchScore = matches[i].score;
        if (options.checkFileExists) { item.existsOnDisk = CheckPathExistsOnDisk(src->filePath); } else { item.existsOnDisk = true; }
        results.push_back(std::move(item));
    }
    return results;
}

// ============================================================================
// GlobalSearchModel Implementation
// ============================================================================

GlobalSearchModel::GlobalSearchModel(uint32_t debounceDelayMs) : debounceDelayMs(debounceDelayMs) {}

void GlobalSearchModel::Open() { isOpen = true; textInput.SelectAll(); }

void GlobalSearchModel::Close() { isOpen = false; pendingDebounce = false; pendingDebounceElapsedMs = 0; }

void GlobalSearchModel::Toggle() {
    if (isOpen) { Close(); } else { Open(); }
}

void GlobalSearchModel::SetQuery(const std::wstring& query, const MediaLibrary& library, bool immediate) {
    currentQuery = query;
    textInput.SetText(WideStringToText16(query));
    ScheduleQuery(library, immediate);
}

void GlobalSearchModel::ScheduleQuery(const MediaLibrary& library, bool immediate) {
    pendingDebounceElapsedMs = 0;
    pendingDebounce = !immediate && debounceDelayMs != 0;
    if (immediate || debounceDelayMs == 0) { ExecuteQueryNow(library); }
}

bool GlobalSearchModel::Tick(uint32_t elapsedMs, const MediaLibrary& library) {
    if (!pendingDebounce) { return false; }
    pendingDebounceElapsedMs += elapsedMs;
    if (pendingDebounceElapsedMs >= debounceDelayMs) {
        pendingDebounce = false;
        pendingDebounceElapsedMs = 0;
        ExecuteQueryNow(library);
        return true;
    }
    return false;
}

void GlobalSearchModel::ExecuteQueryNow(const MediaLibrary& library) {
    currentResults = index.Query(library, currentQuery, queryOptions, currentTotalMatches);
    ClampSelection();
}

void GlobalSearchModel::ClampSelection() {
    if (currentResults.empty()) { selectedIndex = -1; } else if (selectedIndex >= static_cast<int>(currentResults.size())) {
        selectedIndex = static_cast<int>(currentResults.size()) - 1;
    } else if (selectedIndex < 0 && !currentResults.empty()) { selectedIndex = 0; }
}

void GlobalSearchModel::MoveSelectionDown(bool wrap) {
    if (currentResults.empty()) { selectedIndex = -1; return; }
    if (selectedIndex < static_cast<int>(currentResults.size()) - 1) { selectedIndex++; } else if (wrap) { selectedIndex = 0; }
}

void GlobalSearchModel::MoveSelectionUp(bool wrap) {
    if (currentResults.empty()) { selectedIndex = -1; return; }
    if (selectedIndex > 0) { selectedIndex--; } else if (wrap) { selectedIndex = static_cast<int>(currentResults.size()) - 1; }
}

void GlobalSearchModel::MoveSelectionPageDown(size_t pageSize) {
    if (currentResults.empty()) { selectedIndex = -1; return; }
    selectedIndex = (std::min)(static_cast<int>(currentResults.size()) - 1, selectedIndex + static_cast<int>(pageSize));
}

void GlobalSearchModel::MoveSelectionPageUp(size_t pageSize) {
    if (currentResults.empty()) { selectedIndex = -1; return; }
    selectedIndex = (std::max)(0, selectedIndex - static_cast<int>(pageSize));
}

void GlobalSearchModel::MoveSelectionHome() {
    if (currentResults.empty()) { selectedIndex = -1; return; }
    selectedIndex = 0;
}

void GlobalSearchModel::MoveSelectionEnd() {
    if (currentResults.empty()) { selectedIndex = -1; return; }
    selectedIndex = static_cast<int>(currentResults.size()) - 1;
}

void GlobalSearchModel::SetSelectedIndex(int index) {
    if (currentResults.empty()) { selectedIndex = -1; return; }
    selectedIndex = std::clamp(index, -1, static_cast<int>(currentResults.size()) - 1);
}

void GlobalSearchModel::ClearSelection() { selectedIndex = -1; }

GlobalSearchActionResult GlobalSearchModel::ActivateSelection(const MediaLibrary& library) {
    (void)library;
    return BuildSelectionAction(GlobalSearchActionType::PlayMedia);
}

GlobalSearchActionResult GlobalSearchModel::OpenExplorerForSelection(const MediaLibrary& library) {
    (void)library;
    return BuildSelectionAction(GlobalSearchActionType::OpenInExplorer);
}

GlobalSearchActionResult GlobalSearchModel::BuildSelectionAction(GlobalSearchActionType action) const {
    GlobalSearchActionResult result;
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(currentResults.size())) {
        result.action = GlobalSearchActionType::None;
        return result;
    }
    const auto& item = currentResults[static_cast<size_t>(selectedIndex)];
    result.filePath = item.filePath;
    if (action == GlobalSearchActionType::PlayMedia) result.startPosSec = item.lastPosSec;
    bool exists = GlobalSearchIndex::CheckPathExistsOnDisk(item.filePath);
    if (!exists) {
        result.action = GlobalSearchActionType::ReportMissingMedia;
        result.isMissing = true;
        result.message = L"Media file not found on disk: " + item.filePath;
    } else { result.action = action; result.isMissing = false; }
    return result;
}

GlobalSearchProjection GlobalSearchModel::GetProjection() const {
    GlobalSearchProjection proj;
    proj.query = currentQuery;
    proj.results = currentResults;
    proj.totalMatchCount = currentTotalMatches;
    proj.selectedIndex = selectedIndex;
    proj.isDebouncing = pendingDebounce;
    proj.isOpen = isOpen;
    return proj;
}

} // namespace Aperture
