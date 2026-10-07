#include "LibraryQuery.h"
#include "MediaLibrary.h"
#include "Settings.h"
#include "Common.h"

#include <algorithm>
#include <cwctype>
#include <sstream>
#include <numeric>
#include <cmath>
#include <unordered_map>
namespace Aperture {

// ============================================================================
// Helper Utilities
// ============================================================================

namespace {

inline bool IsWhitespaceOrSeparator(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'.' || c == L'_' || c == L'-' || c == L'+';
}

std::wstring ToLowerString(const std::wstring& str) {
    std::wstring result = str;
    for (auto& c : result) { c = std::towlower(c); }
    return result;
}

std::wstring NormalizeDirectoryPath(const std::wstring& path) {
    if (path.empty()) return L"";
    std::wstring result = path;
    for (auto& c : result) { if (c == L'/') c = L'\\'; c = std::towlower(c); }
    // Strip trailing slashes
    while (result.size() > 1 && (result.back() == L'\\' || result.back() == L'/')) { result.pop_back(); }
    return result;
}
std::wstring_view FileNameView(std::wstring_view path) {
    const size_t slash = path.find_last_of(LR"(\/)");
    return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
}

std::wstring_view FileBaseNameView(std::wstring_view name) {
    const size_t dot = name.find_last_of(L'.');
    return dot != std::wstring_view::npos && dot > 0 ? name.substr(0, dot) : name;
}

// Strips brackets like [1080p], (2024), {Erai-raws}, etc.
std::wstring StripBracketedContent(std::wstring_view text) {
    std::wstring result;
    result.reserve(text.size());
    int squareDepth = 0, parenDepth = 0, curlyDepth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t c = text[i];
        if (c == L'[') { squareDepth++; continue; }
        if (c == L']') { if (squareDepth > 0) squareDepth--; continue; }
        if (c == L'(') { parenDepth++; continue; }
        if (c == L')') { if (parenDepth > 0) parenDepth--; continue; }
        if (c == L'{') { curlyDepth++; continue; }
        if (c == L'}') { if (curlyDepth > 0) curlyDepth--; continue; }
        if (squareDepth == 0 && parenDepth == 0 && curlyDepth == 0) { result.push_back(c); }
    }
    return result;
}

} // anonymous namespace

// ============================================================================
// LibraryItemProjection Display Helpers
// ============================================================================

std::wstring LibraryItemProjection::GetFormattedDuration() const {
    if (durationSec <= 0.0) return L"--:--";
    int totalSec = static_cast<int>(durationSec + 0.5);
    int hours = totalSec / 3600;
    int minutes = (totalSec % 3600) / 60;
    int seconds = totalSec % 60;
    wchar_t buf[16];
    if (hours > 0) { swprintf_s(buf, L"%d:%02d:%02d", hours, minutes, seconds); }
    else { swprintf_s(buf, L"%d:%02d", minutes, seconds); }
    return buf;
}

// ============================================================================
// LibraryQueryEngine Implementation
// ============================================================================

namespace {
// Search matching runs once per item per keystroke over the whole library.
// This stack-backed sink keeps that normalization allocation-free for typical
// titles and paths; only unusually long strings fall back to the heap.
struct SearchTextBuffer {
    static constexpr size_t kInlineChars = 256;
    wchar_t inlineBuf[kInlineChars];
    std::wstring heapBuf;
    size_t len = 0;
    void Reset() { len = 0; heapBuf.clear(); }
    void Push(wchar_t c) {
        if (heapBuf.empty() && len < kInlineChars) { inlineBuf[len++] = c; return; }
        if (heapBuf.empty() && len == kInlineChars) { heapBuf.assign(inlineBuf, len); }
        heapBuf.push_back(c);
        ++len;
    }
    std::wstring_view View() const { return heapBuf.empty() ? std::wstring_view(inlineBuf, len) : std::wstring_view(heapBuf); }
};

void NormalizeSearchTextInto(std::wstring_view text, SearchTextBuffer& out) {
    out.Reset();
    if (text.empty()) return;
    bool prevSpace = false;
    for (wchar_t c : text) {
        if (c == L'/' || c == L'\\') c = L'\\';
        if (std::iswspace(c) || c == L'\t' || c == L'\r' || c == L'\n') {
            if (!prevSpace && out.len > 0) { out.Push(L' '); prevSpace = true; }
        } else { out.Push(std::towlower(c)); prevSpace = false; }
    }
    // Trim trailing space
    while (out.len > 0 && out.View().back() == L' ') {
        if (out.heapBuf.empty()) { --out.len; } else { out.heapBuf.pop_back(); --out.len; }
    }
}
std::wstring NormalizeSearchTextFromView(std::wstring_view text) {
    if (text.empty()) return L"";
    std::wstring result;
    result.reserve(text.size());
    bool prevSpace = false;
    for (wchar_t c : text) {
        if (c == L'/' || c == L'\\') c = L'\\';
        if (std::iswspace(c) || c == L'\t' || c == L'\r' || c == L'\n') {
            if (!prevSpace && !result.empty()) { result.push_back(L' '); prevSpace = true; }
        } else { result.push_back(std::towlower(c)); prevSpace = false; }
    }
    // Trim trailing space
    while (!result.empty() && result.back() == L' ') { result.pop_back(); }
    return result;
}

std::wstring SeriesStemFromView(std::wstring_view filename) {
    std::wstring clean = StripBracketedContent(filename);
    // Replace non-alphanumeric separators with spaces
    std::wstring result;
    result.reserve(clean.size());
    bool prevSpace = false;
    for (wchar_t c : clean) {
        c = std::towlower(c);
        if (std::iswalnum(c)) { result.push_back(c); prevSpace = false; } else {
            if (!prevSpace && !result.empty()) { result.push_back(L' '); prevSpace = true; }
        }
    }
    while (!result.empty() && result.back() == L' ') { result.pop_back(); }
    return result;
}

} // namespace

std::wstring LibraryQueryEngine::NormalizeSearchText(const std::wstring& text) { return NormalizeSearchTextFromView(text); }
std::wstring LibraryQueryEngine::ExtractSeriesStem(const std::wstring& filename) { return SeriesStemFromView(filename); }

bool LibraryQueryEngine::AreSameDirectory(const std::wstring& path1, const std::wstring& path2) {
    std::wstring dir1 = NormalizeDirectoryPath(ExtractDirectory(path1));
    std::wstring dir2 = NormalizeDirectoryPath(ExtractDirectory(path2));
    if (dir1.empty() || dir2.empty()) return false;
    return CompareStringOrdinal(dir1.c_str(), -1, dir2.c_str(), -1, TRUE) == CSTR_EQUAL;
}

namespace {
std::wstring CapitalizeWords(const std::wstring& text) {
    std::wstring result = text;
    bool newWord = true;
    for (auto& c : result) {
        if (std::iswspace(c) || c == L'-' || c == L'_' || c == L'.') {
            newWord = true;
        } else if (newWord) { c = std::towupper(c); newWord = false; }
    }
    return result;
}
} // anonymous namespace

namespace {
EpisodeInfo ParseEpisodeInfoImpl(const std::wstring& filePath, bool includeSource) {
    EpisodeInfo info;
    const auto fileName = FileNameView(filePath);
    if (includeSource) {
        info.rawPath = filePath;
        info.rawFileName.assign(fileName);
        info.directoryPath = ExtractDirectory(filePath);
    }
    info.isValid = false;
    // Reject non-supported media extensions
    if (!MediaLibrary::IsSupportedMediaExtension(filePath)) { return info; }
    const auto baseName = FileBaseNameView(fileName);
    std::wstring cleanName = StripBracketedContent(baseName);
    if (cleanName.empty()) { cleanName = baseName; }
    const size_t len = cleanName.size();
    bool foundPattern = false;
    size_t stemEndPos = 0;
    // 1. Check for SxxExx or Sxx.Exx or Sxx_Exx pattern
    for (size_t i = 0; i < len; ++i) {
        if ((cleanName[i] == L's' || cleanName[i] == L'S') && i + 1 < len) {
            // Check if followed by digits
            size_t sDigitsStart = i + 1, sDigitsEnd = sDigitsStart;
            while (sDigitsEnd < len && std::iswdigit(cleanName[sDigitsEnd])) { sDigitsEnd++; }
            if (sDigitsEnd > sDigitsStart) {
                // Now check for 'E' or '.E' or '_E' or '-E'
                size_t ePos = sDigitsEnd;
                while (ePos < len && (cleanName[ePos] == L'.' || cleanName[ePos] == L'_' || cleanName[ePos] == L'-' || cleanName[ePos] == L' ')) {
                    ePos++;
                }
                if (ePos < len && (cleanName[ePos] == L'e' || cleanName[ePos] == L'E') && ePos + 1 < len) {
                    size_t eDigitsStart = ePos + 1, eDigitsEnd = eDigitsStart;
                    while (eDigitsEnd < len && std::iswdigit(cleanName[eDigitsEnd])) { eDigitsEnd++; }
                    if (eDigitsEnd > eDigitsStart) {
                        try {
                            int season = std::stoi(cleanName.substr(sDigitsStart, sDigitsEnd - sDigitsStart));
                            int episode = std::stoi(cleanName.substr(eDigitsStart, eDigitsEnd - eDigitsStart));
                            if (season >= 0 && episode > 0) {
                                info.seasonNumber = season;
                                info.episodeNumber = episode;
                                info.hasSeason = true;
                                info.hasEpisode = true;
                                foundPattern = true;
                                stemEndPos = i;
                                break;
                            }
                        } catch (...) {}
                    }
                }
            }
        }
    }
    // 2. Check for 1x02 or 01x02 pattern
    if (!foundPattern) {
        for (size_t i = 0; i < len; ++i) {
            if ((i == 0 || IsWhitespaceOrSeparator(cleanName[i - 1])) && std::iswdigit(cleanName[i])) {
                size_t sDigitsStart = i, sDigitsEnd = sDigitsStart;
                while (sDigitsEnd < len && std::iswdigit(cleanName[sDigitsEnd])) { sDigitsEnd++; }
                if (sDigitsEnd < len && (cleanName[sDigitsEnd] == L'x' || cleanName[sDigitsEnd] == L'X') && sDigitsEnd + 1 < len) {
                    size_t eDigitsStart = sDigitsEnd + 1, eDigitsEnd = eDigitsStart;
                    while (eDigitsEnd < len && std::iswdigit(cleanName[eDigitsEnd])) { eDigitsEnd++; }
                    if (eDigitsEnd > eDigitsStart) {
                        try {
                            int season = std::stoi(cleanName.substr(sDigitsStart, sDigitsEnd - sDigitsStart));
                            int episode = std::stoi(cleanName.substr(eDigitsStart, eDigitsEnd - eDigitsStart));
                            if (season >= 0 && episode > 0) {
                                info.seasonNumber = season;
                                info.episodeNumber = episode;
                                info.hasSeason = true;
                                info.hasEpisode = true;
                                foundPattern = true;
                                stemEndPos = i;
                                break;
                            }
                        } catch (...) {}
                    }
                }
            }
        }
    }
    // 3. Check for "Episode XX" / "Ep. XX" / "Ep XX" pattern
    if (!foundPattern) {
        std::wstring lowerClean = ToLowerString(cleanName);
        const std::wstring epTokens[] = { L"episode", L"ep.", L"ep" };
        for (const auto& token : epTokens) {
            size_t tokenPos = lowerClean.find(token);
            if (tokenPos != std::wstring::npos) {
                // Must be at start or preceded by separator
                if (tokenPos == 0 || IsWhitespaceOrSeparator(cleanName[tokenPos - 1])) {
                    size_t afterToken = tokenPos + token.size();
                    while (afterToken < len && (IsWhitespaceOrSeparator(cleanName[afterToken]))) { afterToken++; }
                    if (afterToken < len && std::iswdigit(cleanName[afterToken])) {
                        size_t epDigitsEnd = afterToken;
                        while (epDigitsEnd < len && std::iswdigit(cleanName[epDigitsEnd])) { epDigitsEnd++; }
                        try {
                            int episode = std::stoi(cleanName.substr(afterToken, epDigitsEnd - afterToken));
                            if (episode > 0) {
                                info.seasonNumber = 1;
                                info.episodeNumber = episode;
                                info.hasSeason = false;
                                info.hasEpisode = true;
                                foundPattern = true;
                                stemEndPos = tokenPos;
                                break;
                            }
                        } catch (...) {}
                    }
                }
            }
        }
    }
    // 4. Check for hyphen or separator followed by number (e.g. "Show Name - 02")
    if (!foundPattern) {
        for (int i = static_cast<int>(len) - 1; i >= 0; --i) {
            if (std::iswdigit(cleanName[i])) {
                size_t digitsEnd = i + 1, digitsStart = i;
                while (digitsStart > 0 && std::iswdigit(cleanName[digitsStart - 1])) { digitsStart--; }
                // Check preceding separator: e.g. " - " or "_" or " "
                if (digitsStart > 0) {
                    size_t sepPos = digitsStart - 1;
                    while (sepPos > 0 && cleanName[sepPos] == L' ') { sepPos--; }
                    if (cleanName[sepPos] == L'-' || cleanName[sepPos] == L'_' || cleanName[sepPos] == L' ' || cleanName[sepPos] == L'.') {
                        // Ensure it's not a 4-digit year like (2024)
                        size_t digitCount = digitsEnd - digitsStart;
                        if (digitCount <= 3) {
                            try {
                                int episode = std::stoi(cleanName.substr(digitsStart, digitCount));
                                if (episode > 0) {
                                    info.seasonNumber = 1;
                                    info.episodeNumber = episode;
                                    info.hasSeason = false;
                                    info.hasEpisode = true;
                                    foundPattern = true;
                                    stemEndPos = sepPos;
                                    break;
                                }
                            } catch (...) {}
                        }
                    }
                }
                i = static_cast<int>(digitsStart);
            }
        }
    }
    if (foundPattern && info.hasEpisode) {
        info.seriesStem = SeriesStemFromView(std::wstring_view(cleanName).substr(0, stemEndPos));
        if (info.seriesStem.empty()) {
            // Fallback to parent directory stem if filename stem was empty
            if (!includeSource) info.directoryPath = ExtractDirectory(filePath);
            info.seriesStem = SeriesStemFromView(FileNameView(info.directoryPath));
        }
        info.isValid = !info.seriesStem.empty();
    }
    return info;
}

} // namespace

EpisodeInfo LibraryQueryEngine::ParseEpisodeInfo(const std::wstring& filePath) { return ParseEpisodeInfoImpl(filePath, true); }

bool LibraryQueryEngine::MatchesStatusFilter(const MediaItem& item, LibraryStatusFilter filter, bool existsOnDisk) {
    switch (filter) {
        case LibraryStatusFilter::All: return true;
        case LibraryStatusFilter::Unwatched:
            return item.status == "new" || (item.lastPosSec == 0.0 && item.status != "done" && item.status != "in_progress");
        case LibraryStatusFilter::InProgress:
            return item.status == "in_progress" || (item.GetProgressRatio() > 0.0 && item.GetProgressRatio() < 0.95 && item.status != "done");
        case LibraryStatusFilter::Completed: return item.status == "done" || item.GetProgressRatio() >= 0.95;
        case LibraryStatusFilter::Missing: return !existsOnDisk;
    }
    return true;
}

bool LibraryQueryEngine::MatchesSearchQuery(const MediaItem& item, const std::wstring& normalizedQuery) {
    if (normalizedQuery.empty()) return true;
    SearchTextBuffer buffer;
    auto containsNormalized = [&](std::wstring_view text) {
        NormalizeSearchTextInto(text, buffer);
        return buffer.View().find(normalizedQuery) != std::wstring_view::npos;
    };
    const auto displayTitle = item.GetDisplayTitleView();
    if (containsNormalized(displayTitle)) return true;
    if (!item.title.empty() && (displayTitle.data() != item.title.data() || displayTitle.size() != item.title.size()) &&
        containsNormalized(item.title)) return true;
    // Every normalized filename match is also a full-path substring match.
    if (containsNormalized(item.filePath)) return true;
    if (item.overrides.has_value() && item.overrides->series.has_value() && !item.overrides->series->empty()) {
        if (containsNormalized(*item.overrides->series)) return true;
    }
    return false;
}

int LibraryQueryEngine::CompareItems(const MediaItem& a, const MediaItem& b, LibrarySortField sortField, SortDirection direction) {
    int primaryResult = 0;
    switch (sortField) {
        case LibrarySortField::Manual: return 0;
        case LibrarySortField::Title: {
            const auto titleA = a.GetDisplayTitleView();
            const auto titleB = b.GetDisplayTitleView();
            int cmp = CompareStringOrdinal(titleA.data(), -1, titleB.data(), -1, TRUE);
            if (cmp == CSTR_LESS_THAN) primaryResult = -1;
            else if (cmp == CSTR_GREATER_THAN) primaryResult = 1;
            break;
        }
        case LibrarySortField::DateAdded: {
            if (a.addedTimestamp < b.addedTimestamp) primaryResult = -1;
            else if (a.addedTimestamp > b.addedTimestamp) primaryResult = 1;
            break;
        }
        case LibrarySortField::LastPlayed: {
            // If both played or both unplayed, compare timestamps
            if (a.lastWatchedTimestamp == 0 && b.lastWatchedTimestamp > 0) {
                primaryResult = -1; // unplayed comes last
            } else if (a.lastWatchedTimestamp > 0 && b.lastWatchedTimestamp == 0) { primaryResult = 1; } else {
                if (a.lastWatchedTimestamp < b.lastWatchedTimestamp) primaryResult = -1;
                else if (a.lastWatchedTimestamp > b.lastWatchedTimestamp) primaryResult = 1;
            }
            break;
        }
        case LibrarySortField::Duration: {
            if (a.durationSec < b.durationSec) primaryResult = -1;
            else if (a.durationSec > b.durationSec) primaryResult = 1;
            break;
        }
        case LibrarySortField::Progress: {
            double progA = a.GetProgressRatio();
            double progB = b.GetProgressRatio();
            if (progA < progB) primaryResult = -1;
            else if (progA > progB) primaryResult = 1;
            break;
        }
    }
    if (direction == SortDirection::Descending && primaryResult != 0) { primaryResult = -primaryResult; }
    if (primaryResult != 0) return primaryResult;
    // --- Deterministic Tie-Breaking ---
    // 1. Secondary: Title (case-insensitive)
    const auto tA = a.GetTitleView();
    const auto tB = b.GetTitleView();
    int tieTitle = CompareStringOrdinal(tA.data(), -1, tB.data(), -1, TRUE);
    if (tieTitle == CSTR_LESS_THAN) return -1;
    if (tieTitle == CSTR_GREATER_THAN) return 1;
    // 2. Tertiary: FilePath (case-insensitive)
    int tiePath = CompareStringOrdinal(a.filePath.c_str(), -1, b.filePath.c_str(), -1, TRUE);
    if (tiePath == CSTR_LESS_THAN) return -1;
    if (tiePath == CSTR_GREATER_THAN) return 1;
    // 3. Quaternary: ID (lexicographical)
    if (a.id < b.id) return -1;
    if (a.id > b.id) return 1;
    return 0;
}

bool LibraryQueryEngine::IsCacheValid(const MediaLibrary& library, const LibraryQueryOptions& options) const {
    return cachedLibraryVersion == library.GetVersion() && projectionRevisions.SamePlaylistData(library.GetRevisions()) && cachedProjection.options == options &&
           cachedLibraryVersion != (std::numeric_limits<uint64_t>::max)();
}

void LibraryQueryEngine::InvalidateCache() {
    hasView = hasPlan = false;
    cachedPlan = LibraryViewProjection();
    searchFields.clear();
    searchCatalogRevision = (std::numeric_limits<uint64_t>::max)();
    cachedView = LibraryViewProjection();
    cachedLibraryVersion = (std::numeric_limits<uint64_t>::max)();
    cachedProjection = LibraryQueryProjection();
}

LibraryQueryProjection LibraryQueryEngine::ReleaseCachedProjection() {
    LibraryQueryProjection released = std::move(cachedProjection);
    cachedLibraryVersion = (std::numeric_limits<uint64_t>::max)();
    cachedProjection = LibraryQueryProjection();
    return released;
}

bool LibraryQueryEngine::PrimeCache(const MediaLibrary& library, const LibraryQueryOptions& options, LibraryQueryProjection&& projection) {
    if (projection.libraryVersion != library.GetVersion() || projection.options != options) { return false; }
    cachedProjection = std::move(projection);
    cachedLibraryVersion = library.GetVersion();
    projectionRevisions = library.GetRevisions();
    return true;
}

namespace {
bool UsesPlaybackOrder(const LibraryQueryOptions& options) {
    return options.sortField == LibrarySortField::LastPlayed || options.sortField == LibrarySortField::Progress ||
        options.groupMode == LibraryGroupMode::Duplicates;
}
bool SameQueryData(const LibraryRevisions& a, const LibraryRevisions& b, const LibraryQueryOptions& options, bool plan) {
    return a.catalog == b.catalog &&
        (options.groupMode != LibraryGroupMode::Flat || a.playlists == b.playlists) &&
        (!(UsesPlaybackOrder(options) || (!plan && ((options.pinLastWatched && options.groupMode == LibraryGroupMode::Flat) || options.statusFilter != LibraryStatusFilter::All))) ||
            a.playback == b.playback);
}
}

const LibraryViewProjection& LibraryQueryEngine::QueryView(const MediaLibrary& library, const LibraryQueryOptions& options,
    const std::function<bool(const std::wstring&)>& fileExistsFn) {
    if (hasView && SameQueryData(viewRevisions, library.GetRevisions(), options, false) && cachedView.options == options) {
        cachedView.libraryVersion = library.GetVersion();
        return cachedView;
    }
    LibraryViewProjection next;
    // Filesystem-sensitive queries retain their explicit probe path. They must
    // not seed the reusable, disk-independent ordering plan.
    if (options.checkFileExists || options.statusFilter == LibraryStatusFilter::Missing) {
        next = BuildView(library, options, fileExistsFn);
    } else {
        LibraryQueryOptions planOptions = options;
        planOptions.searchQuery.clear();
        planOptions.statusFilter = LibraryStatusFilter::All;
        planOptions.maxItems = 0;
        planOptions.pinLastWatched = false;
        planOptions.collapsedGroups.reset();
        const bool needsPlan = !hasPlan || !SameQueryData(planRevisions, library.GetRevisions(), planOptions, true) ||
            cachedPlan.options != planOptions;
        // A cold, bounded request can use partial sorting/early termination.
        // Once an unlimited view prepared this order, caps can reuse it too.
        if (needsPlan && options.maxItems) {
            next = BuildView(library, options, nullptr);
        } else {
            if (needsPlan) {
                cachedPlan = BuildView(library, planOptions, nullptr);
                planRevisions = library.GetRevisions();
                hasPlan = true;
                ++planBuildCount;
            }
            next = FilterPlan(library, options);
        }
    }
    const bool sameShape = hasView && cachedView.options.groupMode == next.options.groupMode &&
        cachedView.indices == next.indices && cachedView.groups.size() == next.groups.size() &&
        std::equal(cachedView.groups.begin(), cachedView.groups.end(), next.groups.begin(),
            [](const LibraryGroupSummary& a, const LibraryGroupSummary& b) {
                return a.key == b.key && a.startIndex == b.startIndex && a.count == b.count && a.isCollapsed == b.isCollapsed;
            });
    next.generation = sameShape ? cachedView.generation : ++viewGeneration;
    cachedView = std::move(next);
    viewRevisions = library.GetRevisions();
    hasView = true;
    ++viewBuildCount;
    return cachedView;
}

LibraryViewProjection LibraryQueryEngine::FilterPlan(const MediaLibrary& library, const LibraryQueryOptions& options) {
    LibraryViewProjection result;
    result.options = options;
    result.libraryVersion = library.GetVersion();
    result.totalLibraryCount = library.mediaItems.size();
    const std::wstring query = NormalizeSearchText(options.searchQuery);
    if (!query.empty() && (searchCatalogRevision != library.GetRevisions().catalog || searchFields.size() != library.mediaItems.size())) {
        searchFields.clear();
        searchFields.reserve(library.mediaItems.size());
        for (const auto& item : library.mediaItems) {
            SearchFields fields;
            fields.title = NormalizeSearchText(std::wstring(item.GetDisplayTitleView()));
            if (item.GetDisplayTitleView() != item.title) fields.originalTitle = NormalizeSearchText(item.title);
            fields.path = NormalizeSearchText(item.filePath);
            if (item.overrides && item.overrides->series) fields.series = NormalizeSearchText(*item.overrides->series);
            searchFields.push_back(std::move(fields));
        }
        searchCatalogRevision = library.GetRevisions().catalog;
        ++searchTextBuildCount;
    }
    const auto matches = [&](size_t index) {
        if (!MatchesStatusFilter(library.mediaItems[index], options.statusFilter, true)) return false;
        if (query.empty()) return true;
        const auto& fields = searchFields[index];
        return fields.title.find(query) != std::wstring::npos || fields.originalTitle.find(query) != std::wstring::npos ||
            fields.path.find(query) != std::wstring::npos || fields.series.find(query) != std::wstring::npos;
    };
    const bool filtered = !query.empty() || options.statusFilter != LibraryStatusFilter::All;
    std::vector<uint8_t> eligible;
    if (filtered) {
        eligible.resize(library.mediaItems.size());
        // Duplicate filteredCount includes matching records outside duplicate groups.
        if (options.groupMode == LibraryGroupMode::Duplicates) {
            for (size_t i = 0; i < eligible.size(); ++i) { eligible[i] = matches(i); result.filteredCount += eligible[i]; }
        } else {
            for (size_t index : cachedPlan.indices) { eligible[index] = matches(index); result.filteredCount += eligible[index]; }
        }
    } else { result.filteredCount = cachedPlan.filteredCount; }
    const auto included = [&](size_t index) { return !filtered || eligible[index] != 0; };
    size_t capacity = cachedPlan.indices.size();
    if (options.groupMode != LibraryGroupMode::Flat && options.collapsedGroups && !options.collapsedGroups->empty()) {
        capacity = 0;
        for (const auto& group : cachedPlan.groups) if (!options.CollapsedGroupCount(group.key)) capacity += group.count;
    }
    if (filtered) capacity = (std::min)(capacity, result.filteredCount);
    result.indices.reserve(options.maxItems ? (std::min)(options.maxItems, capacity) : capacity);
    if (options.groupMode == LibraryGroupMode::Flat) {
        size_t pinned = library.mediaItems.size();
        if (options.pinLastWatched) {
            for (size_t index : cachedPlan.indices) {
                const auto timestamp = library.mediaItems[index].lastWatchedTimestamp;
                if (included(index) && timestamp > 0 && (pinned == library.mediaItems.size() ||
                    timestamp > library.mediaItems[pinned].lastWatchedTimestamp ||
                    (timestamp == library.mediaItems[pinned].lastWatchedTimestamp && index < pinned))) pinned = index;
            }
        }
        if (pinned != library.mediaItems.size()) result.indices.push_back(pinned);
        for (size_t index : cachedPlan.indices) {
            if (!included(index) || index == pinned) continue;
            if (options.maxItems && result.indices.size() >= options.maxItems) break;
            result.indices.push_back(index);
        }
        result.isCapped = result.indices.size() < result.filteredCount;
        return result;
    }
    result.groups.reserve(cachedPlan.groups.size());
    const bool duplicates = options.groupMode == LibraryGroupMode::Duplicates;
    for (const auto& source : cachedPlan.groups) {
        if (!filtered && !options.maxItems) {
            LibraryGroupSummary group = source;
            group.startIndex = result.indices.size();
            group.isCollapsed = options.CollapsedGroupCount(group.key);
            if (!group.isCollapsed) result.indices.insert(result.indices.end(), cachedPlan.indices.begin() + source.startIndex,
                cachedPlan.indices.begin() + source.startIndex + source.count);
            result.groups.push_back(std::move(group));
            continue;
        }
        size_t count = 0, firstOriginal = library.mediaItems.size();
        const size_t end = source.startIndex + source.count;
        for (size_t row = source.startIndex; row < end; ++row) {
            const size_t index = cachedPlan.indices[row];
            if (included(index)) { ++count; firstOriginal = (std::min)(firstOriginal, index); }
        }
        if (!count || (duplicates && filtered && count < 2)) continue;
        LibraryGroupSummary group = source;
        group.startIndex = result.indices.size();
        group.count = duplicates ? 0 : count;
        group.totalDurationSec = 0;
        group.totalSizeBytes = 0;
        group.isCollapsed = options.CollapsedGroupCount(group.key);
        // Preserve labels from the first matching source record, including casing.
        if (filtered && options.groupMode == LibraryGroupMode::Folder) {
            group.subtitle = ExtractDirectory(library.mediaItems[firstOriginal].filePath);
            group.title = group.subtitle.empty() ? L"Root" : GetFileNameFromPath(group.subtitle);
            if (group.title.empty()) group.title = group.subtitle;
        } else if (filtered && options.groupMode == LibraryGroupMode::Series) {
            const auto& overrides = library.mediaItems[firstOriginal].overrides;
            if (group.key == L"__standalone__") {
                const bool special = !(overrides && overrides->series && !overrides->series->empty());
                // A user override can equal the reserved standalone key.
                // Its first matching record then changes group ordering rules.
                if (special != source.isSpecial) return BuildView(library, options, nullptr);
            }
            group.title = overrides && overrides->series && !overrides->series->empty() ? *overrides->series :
                (group.isSpecial ? L"Standalone Videos" : CapitalizeWords(group.key));
        }
        for (size_t row = source.startIndex; row < end; ++row) {
            const size_t index = cachedPlan.indices[row];
            if (!included(index)) continue;
            const auto& item = library.mediaItems[index];
            group.totalDurationSec += item.durationSec;
            group.totalSizeBytes += item.fileSizeBytes;
            if (duplicates) ++group.count;
            if (!group.isCollapsed) {
                if (options.maxItems && result.indices.size() >= options.maxItems) { result.isCapped = true; break; }
                result.indices.push_back(index);
            }
        }
        result.groups.push_back(std::move(group));
        if (options.maxItems && result.indices.size() >= options.maxItems) break;
    }
    return result;
}

LibraryViewProjection LibraryQueryEngine::ReleaseCachedView() {
    auto result = std::move(cachedView);
    InvalidateCache();
    return result;
}

bool LibraryQueryEngine::PrimeViewCache(const MediaLibrary& library, const LibraryQueryOptions& options,
    LibraryViewProjection&& projection) {
    if (projection.libraryVersion != library.GetVersion() || projection.options != options) return false;
    InvalidateCache();
    cachedView = std::move(projection);
    viewRevisions = library.GetRevisions();
    hasView = true;
    cachedView.generation = ++viewGeneration;
    if (options.searchQuery.empty() && options.statusFilter == LibraryStatusFilter::All && !options.checkFileExists &&
        !options.pinLastWatched && !options.maxItems && (!options.collapsedGroups || options.collapsedGroups->empty())) {
        cachedPlan = cachedView;
        planRevisions = viewRevisions;
        hasPlan = true;
    }
    return true;
}

const LibraryQueryProjection& LibraryQueryEngine::Query(const MediaLibrary& library, const LibraryQueryOptions& options,
    const std::function<bool(const std::wstring&)>& fileExistsFn) {
    if (IsCacheValid(library, options)) return cachedProjection;
    const auto& view = QueryView(library, options, fileExistsFn);
    LibraryQueryProjection projection;
    projection.groups = view.groups;
    projection.options = view.options;
    projection.totalLibraryCount = view.totalLibraryCount;
    projection.filteredCount = view.filteredCount;
    projection.libraryVersion = library.GetVersion();
    projection.isCapped = view.isCapped;
    projection.items.reserve(view.indices.size());
    for (size_t i = 0; i < view.indices.size(); ++i) {
        const size_t index = view.indices[i];
        const auto& item = library.mediaItems[index];
        LibraryItemProjection projected;
        projected.id = item.id;
        projected.filePath = item.filePath;
        projected.title = item.GetDisplayTitle();
        projected.durationSec = item.durationSec;
        projected.lastPosSec = item.lastPosSec;
        projected.status = item.status;
        projected.progressRatio = item.GetProgressRatio();
        projected.addedTimestamp = item.addedTimestamp;
        projected.lastWatchedTimestamp = item.lastWatchedTimestamp;
        projected.fileSizeBytes = item.fileSizeBytes;
        projected.existsOnDisk = view.availability.empty() || view.availability[i] != 0;
        projected.originalLibraryIndex = index;
        projection.items.push_back(std::move(projected));
    }
    cachedProjection = std::move(projection);
    cachedLibraryVersion = library.GetVersion();
    projectionRevisions = library.GetRevisions();
    return cachedProjection;
}

LibraryViewProjection LibraryQueryEngine::BuildView( const MediaLibrary& library, const LibraryQueryOptions& options,
    const std::function<bool(const std::wstring&)>& fileExistsFn) {
    LibraryViewProjection projection;
    projection.options = options;
    projection.totalLibraryCount = library.mediaItems.size();
    projection.libraryVersion = library.GetVersion();
    std::wstring normQuery = NormalizeSearchText(options.searchQuery);
    bool checkDisk = options.checkFileExists || (options.statusFilter == LibraryStatusFilter::Missing);
    struct ItemMatch {
        const MediaItem* item = nullptr;
        size_t originalIndex = 0;
        bool existsOnDisk = true;
        int seasonNumber = 1, episodeNumber = 0;
    };
    // Matches only retain data needed by sorting/projection. Parsed episode
    // strings die after classification; shared labels belong to their group.
    struct GroupBucket {
        std::wstring key, title, subtitle;
        bool isSpecial = false;
        size_t customOrder = (std::numeric_limits<size_t>::max)();
        std::vector<ItemMatch> items;
    };
    std::vector<GroupBucket> buckets;
    std::unordered_map<std::wstring, size_t> groupKeyToIndex;
    auto projectItem = [&](const ItemMatch& match) {
        projection.indices.push_back(match.originalIndex);
        if (checkDisk) projection.availability.push_back(match.existsOnDisk ? 1 : 0);
    };
    std::vector<ItemMatch> matches;
    if (options.groupMode == LibraryGroupMode::Flat) matches.reserve(library.mediaItems.size());
    // Duplicate groups reuse the first filtering pass, including its disk
    // result. No second filesystem probe or full ItemMatch array is needed.
    std::vector<uint8_t> duplicateEligibility;
    if (options.groupMode == LibraryGroupMode::Duplicates) duplicateEligibility.resize(library.mediaItems.size());
    std::unordered_set<std::wstring> playlistItemPaths;
    if (options.groupMode == LibraryGroupMode::Flat) {
        playlistItemPaths.reserve(128);
        for (const auto& pl : library.playlists) {
            if (pl.IsSmart()) continue;
            for (const auto& p : pl.itemPaths) { playlistItemPaths.insert(NormalizePathKey(p)); }
        }
    }
    for (size_t i = 0; i < library.mediaItems.size(); ++i) {
        const auto& item = library.mediaItems[i];
        if (!playlistItemPaths.empty()) {
            if (playlistItemPaths.find(NormalizePathKey(item.filePath)) != playlistItemPaths.end()) { continue; }
        }
        bool exists = true;
        if (checkDisk) { exists = fileExistsFn ? fileExistsFn(item.filePath) : FileExists(item.filePath); }
        if (!MatchesStatusFilter(item, options.statusFilter, exists)) { continue; }
        if (!MatchesSearchQuery(item, normQuery)) { continue; }
        ++projection.filteredCount;
        if (options.groupMode == LibraryGroupMode::Duplicates) {
            duplicateEligibility[i] = exists ? 1 : 2;
            continue;
        }
        ItemMatch m;
        m.item = &item;
        m.originalIndex = i;
        m.existsOnDisk = exists;
        if (options.groupMode == LibraryGroupMode::Flat) {
            matches.push_back(m);
            continue;
        }
        std::wstring groupKey, directory;
        const std::wstring* seriesOverride = nullptr;
        bool isSpecialGroup = false;
        if (options.groupMode == LibraryGroupMode::Folder) {
            directory = ExtractDirectory(item.filePath);
            groupKey = NormalizeDirectoryPath(directory);
        } else if (options.groupMode == LibraryGroupMode::Series) {
            const EpisodeInfo episode = ParseEpisodeInfoImpl(item.filePath, false);
            m.seasonNumber = episode.seasonNumber;
            m.episodeNumber = episode.episodeNumber;
            if (item.overrides.has_value() && item.overrides->series.has_value() && !item.overrides->series->empty()) {
                groupKey = ToLowerString(*item.overrides->series);
                seriesOverride = &*item.overrides->series;
            } else {
                isSpecialGroup = !episode.isValid || episode.seriesStem.empty();
                groupKey = isSpecialGroup ? L"__standalone__" : episode.seriesStem;
            }
        }
        // Most items reuse an existing group. Construct its labels and map
        // node only once, rather than allocating and discarding them per item.
        auto group = groupKeyToIndex.find(groupKey);
        if (group == groupKeyToIndex.end()) {
            GroupBucket bucket;
            if (options.groupMode == LibraryGroupMode::Folder) {
                bucket.title = directory.empty() ? L"Root" : GetFileNameFromPath(directory);
                if (bucket.title.empty()) bucket.title = directory;
                bucket.subtitle = std::move(directory);
            } else {
                bucket.title = seriesOverride ? *seriesOverride :
                    (isSpecialGroup ? L"Standalone Videos" : CapitalizeWords(groupKey));
                bucket.subtitle = isSpecialGroup ? L"Standalone videos & other media" : L"Series";
            }
            bucket.key = groupKey;
            bucket.isSpecial = isSpecialGroup;
            group = groupKeyToIndex.emplace(std::move(groupKey), buckets.size()).first;
            buckets.push_back(std::move(bucket));
        }
        buckets[group->second].items.push_back(m);
    }
    if (options.groupMode == LibraryGroupMode::Duplicates) {
        DuplicateDetectionResult dupResult = DetectDuplicates(library);
        projection.groups.reserve(dupResult.groups.size());
        for (const auto& dgrp : dupResult.groups) {
            std::vector<ItemMatch> groupMatches;
            for (size_t idxInGrp = 0; idxInGrp < dgrp.items.size(); ++idxInGrp) {
                const auto* itemPtr = dgrp.items[idxInGrp];
                size_t origIdx = dgrp.originalIndices[idxInGrp];
                if (duplicateEligibility[origIdx] == 0) continue;
                ItemMatch m;
                m.item = itemPtr;
                m.originalIndex = origIdx;
                m.existsOnDisk = duplicateEligibility[origIdx] == 1;
                groupMatches.push_back(std::move(m));
            }
            if (groupMatches.size() < 2) {
                if (!normQuery.empty() || options.statusFilter != LibraryStatusFilter::All) { continue; }
            }
            if (groupMatches.empty()) continue;
            LibraryGroupSummary grp;
            grp.key = dgrp.key;
            grp.title = (dgrp.matchKind == DuplicateMatchKind::LikelyContent ? L"Likely Duplicate: " : L"Exact Duplicate: ") + dgrp.title;
            grp.subtitle = dgrp.subtitle;
            grp.isSpecial = (dgrp.matchKind == DuplicateMatchKind::LikelyContent);
            grp.startIndex = projection.indices.size();
            grp.count = 0;
            grp.isCollapsed = options.CollapsedGroupCount(dgrp.key);
            for (const auto& m : groupMatches) {
                const auto& item = *m.item;
                grp.totalSizeBytes += item.fileSizeBytes;
                grp.totalDurationSec += item.durationSec;
                grp.count++;
                if (!grp.isCollapsed) {
                    if (options.maxItems > 0 && projection.indices.size() >= options.maxItems) { projection.isCapped = true; break; }
                    projectItem(m);
                }
            }
            if (grp.count > 0) { projection.groups.push_back(std::move(grp)); }
            if (options.maxItems > 0 && projection.indices.size() >= options.maxItems) { break; }
        }
    } else if (options.groupMode == LibraryGroupMode::Flat) {
        auto sortBegin = matches.begin();
        if (options.pinLastWatched) {
            auto best = std::max_element(matches.begin(), matches.end(), [](const ItemMatch& a, const ItemMatch& b) {
                    return a.item->lastWatchedTimestamp < b.item->lastWatchedTimestamp;
                });
            if (best != matches.end() && best->item->lastWatchedTimestamp > 0) {
                if (best != matches.begin()) { std::rotate(matches.begin(), best, best + 1); }
                ++sortBegin;
            }
        }
        size_t countToProject = matches.size();
        if (options.maxItems > 0 && countToProject > options.maxItems) {
            countToProject = options.maxItems;
            projection.isCapped = true;
        } else { projection.isCapped = false; }
        if (options.sortField != LibrarySortField::Manual) {
            const auto less = [&](const ItemMatch& a, const ItemMatch& b) {
                const int cmp = CompareItems(*a.item, *b.item, options.sortField, options.sortDirection);
                return cmp != 0 ? cmp < 0 : a.originalIndex < b.originalIndex;
            };
            if (projection.isCapped && countToProject < matches.size() / 4) {
                // Input indices preserve stable-sort ties, including after
                // rotating the pinned item out of the sortable suffix.
                std::partial_sort(sortBegin, matches.begin() + countToProject, matches.end(),
                    [&](const ItemMatch& a, const ItemMatch& b) {
                        const int cmp = CompareItems(*a.item, *b.item, options.sortField, options.sortDirection);
                        return cmp != 0 ? cmp < 0 : a.originalIndex < b.originalIndex;
                    });
            } else {
                std::stable_sort(sortBegin, matches.end(), less);
            }
        }
        projection.indices.reserve(countToProject);
        for (size_t i = 0; i < countToProject; ++i) { const auto& m = matches[i]; projectItem(m); }
    } else {
        // Grouped Mode (Folder or Series)
        if (options.groupMode == LibraryGroupMode::Folder && !options.FolderOrder().empty()) {
            std::unordered_map<std::wstring, size_t> folderOrder;
            folderOrder.reserve(options.FolderOrder().size());
            for (size_t i = 0; i < options.FolderOrder().size(); ++i) {
                // The first occurrence wins, matching the old linear lookup.
                folderOrder.emplace(NormalizeDirectoryPath(options.FolderOrder()[i]), i);
            }
            for (auto& bucket : buckets) {
                // Folder keys are normalized when the buckets are built.
                auto order = folderOrder.find(bucket.key);
                if (order != folderOrder.end()) bucket.customOrder = order->second;
            }
        }
        // Sort groups deterministically (respecting customFolderOrder if set)
        std::sort(buckets.begin(), buckets.end(), [&](const GroupBucket& a, const GroupBucket& b) {
            if (a.isSpecial != b.isSpecial) {
                return !a.isSpecial; // non-special first
            }
            if (a.customOrder != b.customOrder) return a.customOrder < b.customOrder;
            int cmp = CompareStringOrdinal(a.title.c_str(), -1, b.title.c_str(), -1, TRUE);
            if (cmp == CSTR_LESS_THAN) return true;
            if (cmp == CSTR_GREATER_THAN) return false;
            return a.key < b.key;
        });
        // Sort only groups that will be emitted; capped queries can stop early.
        auto sortBucket = [&](GroupBucket& bucket) {
            if (options.groupMode == LibraryGroupMode::Series && !bucket.isSpecial) {
                // In series mode, sort by season and episode number, with CompareItems as tie-breaker
                std::sort(bucket.items.begin(), bucket.items.end(), [&](const ItemMatch& a, const ItemMatch& b) {
                    if (a.seasonNumber != b.seasonNumber) {
                        return options.sortDirection == SortDirection::Descending ? a.seasonNumber > b.seasonNumber
                            : a.seasonNumber < b.seasonNumber;
                    }
                    if (a.episodeNumber != b.episodeNumber) {
                        return options.sortDirection == SortDirection::Descending
                            ? a.episodeNumber > b.episodeNumber
                            : a.episodeNumber < b.episodeNumber;
                    }
                    const int cmp = CompareItems(*a.item, *b.item, options.sortField, options.sortDirection);
                    return cmp != 0 ? cmp < 0 : a.originalIndex < b.originalIndex;
                });
            } else {
                // Folder mode or standalone series group: sort by active sortField & sortDirection
                std::sort(bucket.items.begin(), bucket.items.end(), [&](const ItemMatch& a, const ItemMatch& b) {
                    const int cmp = CompareItems(*a.item, *b.item, options.sortField, options.sortDirection);
                    return cmp != 0 ? cmp < 0 : a.originalIndex < b.originalIndex;
                });
            }
        };
        // Flatten groups into projection.indices and record groups
        projection.groups.reserve(buckets.size());
        size_t visibleCount = 0;
        for (const auto& bucket : buckets) {
            if (!options.CollapsedGroupCount(bucket.key)) visibleCount += bucket.items.size();
        }
        projection.indices.reserve(options.maxItems > 0 ? (std::min)(visibleCount, options.maxItems) : visibleCount);
        for (auto& bucket : buckets) {
            if (bucket.items.empty()) continue;
            sortBucket(bucket);
            LibraryGroupSummary grp;
            grp.key = bucket.key;
            grp.title = bucket.title;
            grp.subtitle = bucket.subtitle;
            grp.isSpecial = bucket.isSpecial;
            grp.startIndex = projection.indices.size();
            grp.count = bucket.items.size();
            grp.isCollapsed = options.CollapsedGroupCount(bucket.key);
            for (const auto& m : bucket.items) {
                const auto& item = *m.item;
                grp.totalSizeBytes += item.fileSizeBytes;
                grp.totalDurationSec += item.durationSec;
                if (!grp.isCollapsed) {
                    if (options.maxItems > 0 && projection.indices.size() >= options.maxItems) { projection.isCapped = true; break; }
                    projectItem(m);
                }
            }
            projection.groups.push_back(std::move(grp));
            if (options.maxItems > 0 && projection.indices.size() >= options.maxItems) { break; }
        }
    }
    return projection;
}

// ============================================================================
// Conservative Duplicate Detection Implementation
// ============================================================================

DuplicateDetectionResult LibraryQueryEngine::DetectDuplicates(const std::vector<MediaItem>& items) {
    DuplicateDetectionResult result;
    const size_t count = items.size();
    if (count < 2) return result;
    std::vector<bool> assigned(count);
    size_t unassignedCount = count;
    auto populateGroup = [&](DuplicateGroup& group, auto first, auto last, auto indexOf) -> const MediaItem& {
        group.originalIndices.reserve(static_cast<size_t>(std::distance(first, last)));
        for (; first != last; ++first) group.originalIndices.push_back(indexOf(*first));
        std::sort(group.originalIndices.begin(), group.originalIndices.end(), [&](size_t a, size_t b) {
            const auto& left = items[a]; const auto& right = items[b];
            const bool playedA = left.status == "done" || left.lastPosSec > 0.0;
            const bool playedB = right.status == "done" || right.lastPosSec > 0.0;
            if (playedA != playedB) return playedA > playedB;
            const double progressA = left.GetProgressRatio(), progressB = right.GetProgressRatio();
            if (progressA != progressB) return progressA > progressB;
            if (left.addedTimestamp != right.addedTimestamp && left.addedTimestamp > 0 && right.addedTimestamp > 0)
                return left.addedTimestamp < right.addedTimestamp;
            if (left.filePath.size() != right.filePath.size()) return left.filePath.size() < right.filePath.size();
            const int cmp = CompareStringOrdinal(left.filePath.c_str(), -1, right.filePath.c_str(), -1, TRUE);
            if (cmp == CSTR_LESS_THAN) return true;
            if (cmp == CSTR_GREATER_THAN) return false;
            return left.id < right.id;
        });
        group.items.reserve(group.originalIndices.size());
        for (const size_t index : group.originalIndices) { assigned[index] = true; --unassignedCount; group.items.push_back(&items[index]); }
        const auto& keeper = *group.items.front();
        group.title = keeper.title.empty() ? GetFileNameFromPath(keeper.filePath) : keeper.title;
        group.individualSizeBytes = keeper.fileSizeBytes;
        group.totalDuplicateSizeBytes = (group.items.size() - 1) * keeper.fileSizeBytes;
        group.durationSec = keeper.durationSec;
        return keeper;
    };
    // Sort compact arrays instead of allocating a node/vector for each unique
    // identity/path. Each phase releases its temporary keys before the next.
    {
        std::vector<size_t> identities;
        for (size_t i = 0; i < count; ++i)
            if (items[i].volumeSerialNumber && items[i].fileIndex) identities.push_back(i);
        std::sort(identities.begin(), identities.end(), [&](size_t a, size_t b) {
            if (items[a].volumeSerialNumber != items[b].volumeSerialNumber) return items[a].volumeSerialNumber < items[b].volumeSerialNumber;
            if (items[a].fileIndex != items[b].fileIndex) return items[a].fileIndex < items[b].fileIndex;
            return a < b;
        });
        for (size_t first = 0; first < identities.size();) {
            size_t last = first + 1;
            const auto& item = items[identities[first]];
            while (last < identities.size() && items[identities[last]].volumeSerialNumber == item.volumeSerialNumber &&
                   items[identities[last]].fileIndex == item.fileIndex) ++last;
            if (last - first > 1) {
                DuplicateGroup group; group.matchKind = DuplicateMatchKind::ExactFileIdentity;
                group.key = L"dup_exact_fid_" + std::to_wstring(item.volumeSerialNumber) + L"_" + std::to_wstring(item.fileIndex);
                populateGroup(group, identities.begin() + first, identities.begin() + last, [](size_t index) { return index; });
                group.subtitle = L"Exact file identity (" + std::to_wstring(group.items.size()) + L" entries • matching volume & file index)";
                result.groups.push_back(std::move(group));
            }
            first = last;
        }
    }
    {
        struct PathAlias { std::wstring key; size_t index; };
        std::vector<PathAlias> paths; paths.reserve(unassignedCount);
        for (size_t i = 0; i < count; ++i) {
            if (assigned[i]) continue;
            auto key = NormalizePathKey(items[i].filePath);
            if (!key.empty()) paths.push_back({std::move(key), i});
        }
        std::sort(paths.begin(), paths.end(), [](const PathAlias& a, const PathAlias& b) {
            return a.key != b.key ? a.key < b.key : a.index < b.index;
        });
        for (size_t first = 0; first < paths.size();) {
            size_t last = first + 1;
            while (last < paths.size() && paths[last].key == paths[first].key) ++last;
            if (last - first > 1) {
                DuplicateGroup group; group.matchKind = DuplicateMatchKind::ExactPathAlias;
                group.key = L"dup_exact_path_" + paths[first].key;
                populateGroup(group, paths.begin() + first, paths.begin() + last, [](const PathAlias& path) { return path.index; });
                group.subtitle = L"Exact path alias (" + std::to_wstring(group.items.size()) + L" entries • same normalized location)";
                result.groups.push_back(std::move(group));
            }
            first = last;
        }
    }
    std::vector<size_t> candidates; candidates.reserve(unassignedCount);
    for (size_t i = 0; i < count; ++i)
        if (!assigned[i] && items[i].fileSizeBytes > 0 && items[i].durationSec > 0.5) candidates.push_back(i);
    // A negative root value is its component size; nonnegative values are
    // parent indices. One array supports weighted union and iterative compression.
    std::vector<std::ptrdiff_t> parent(candidates.size(), -1);
    auto rootOf = [&](size_t index) {
        size_t root = index;
        while (parent[root] >= 0) root = static_cast<size_t>(parent[root]);
        while (parent[index] >= 0) {
            const size_t next = static_cast<size_t>(parent[index]);
            parent[index] = static_cast<std::ptrdiff_t>(root); index = next;
        }
        return root;
    };
    auto unite = [&](size_t a, size_t b) {
        a = rootOf(a); b = rootOf(b);
        if (a == b) return;
        if (parent[a] > parent[b]) std::swap(a, b);
        parent[a] += parent[b]; parent[b] = static_cast<std::ptrdiff_t>(a);
    };
    auto itemFor = [&](size_t candidate) -> const MediaItem& { return items[candidates[candidate]]; };
    auto knownResolution = [](const MediaItem& item) { return item.videoWidth > 0 && item.videoHeight > 0; };
    auto sameResolution = [&](size_t a, size_t b) {
        const auto& left = itemFor(a); const auto& right = itemFor(b);
        const bool knownA = knownResolution(left), knownB = knownResolution(right);
        return knownA == knownB && (!knownA || (left.videoWidth == right.videoWidth && left.videoHeight == right.videoHeight));
    };
    auto resolutionThenDuration = [&](size_t a, size_t b) {
        const auto& left = itemFor(a); const auto& right = itemFor(b);
        const bool knownA = knownResolution(left), knownB = knownResolution(right);
        if (knownA != knownB) return knownA < knownB;
        if (knownA) {
            if (left.videoWidth != right.videoWidth) return left.videoWidth < right.videoWidth;
            if (left.videoHeight != right.videoHeight) return left.videoHeight < right.videoHeight;
        }
        return left.durationSec != right.durationSec ? left.durationSec < right.durationSec : a < b;
    };
    std::vector<size_t> ordered(candidates.size());
    std::iota(ordered.begin(), ordered.end(), size_t{0});
    std::sort(ordered.begin(), ordered.end(), [&](size_t a, size_t b) {
        const auto& left = itemFor(a); const auto& right = itemFor(b);
        if (left.fileSizeBytes != right.fileSizeBytes) return left.fileSizeBytes < right.fileSizeBytes;
        return left.durationSec != right.durationSec ? left.durationSec < right.durationSec : a < b;
    });
    struct TextKeys { std::wstring stem, title, file; };
    auto textKeysFor = [&](const MediaItem& item) {
        const auto name = FileNameView(item.filePath);
        return TextKeys{SeriesStemFromView(name), NormalizeSearchTextFromView(item.title.empty() ? name : std::wstring_view(item.title)),
            NormalizeSearchTextFromView(FileBaseNameView(name))};
    };
    for (size_t first = 0; first < ordered.size();) {
        size_t last = first + 1;
        // A duration gap over 0.5 separates components even when sizes match.
        while (last < ordered.size() && itemFor(ordered[last]).fileSizeBytes == itemFor(ordered[first]).fileSizeBytes &&
               !(itemFor(ordered[last]).durationSec - itemFor(ordered[last - 1]).durationSec > 0.5)) ++last;
        if (last - first < 2) { first = last; continue; }
        if (last - first == 2) {
            const size_t a = (std::min)(ordered[first], ordered[first + 1]), b = (std::max)(ordered[first], ordered[first + 1]);
            const auto& left = itemFor(a); const auto& right = itemFor(b);
            const bool known = knownResolution(left) && knownResolution(right);
            if (!known || sameResolution(a, b)) {
                if (known && left.durationSec >= 5.0) { unite(a, b); }
                else {
                    const auto leftKeys = textKeysFor(left), rightKeys = textKeysFor(right);
                    if ((!leftKeys.stem.empty() && leftKeys.stem == rightKeys.stem) ||
                        (!leftKeys.title.empty() && leftKeys.title == rightKeys.title) ||
                        (!leftKeys.file.empty() && leftKeys.file == rightKeys.file)) unite(a, b);
                }
            }
            first = last; continue;
        }
        std::vector<size_t> known;
        bool needsText = false;
        for (size_t i = first; i < last; ++i) {
            const auto& item = itemFor(ordered[i]);
            if (knownResolution(item)) known.push_back(ordered[i]);
            if (!knownResolution(item) || item.durationSec < 5.0) needsText = true;
        }
        std::sort(known.begin(), known.end(), resolutionThenDuration);
        for (size_t begin = 0; begin < known.size();) {
            size_t end = begin + 1;
            while (end < known.size() && sameResolution(known[begin], known[end])) ++end;
            size_t high = begin;
            while (high < end && itemFor(known[high]).durationSec < 5.0) ++high;
            for (size_t i = high + 1; i < end; ++i)
                if (!(itemFor(known[i]).durationSec - itemFor(known[i - 1]).durationSec > 0.5)) unite(known[i - 1], known[i]);
            // Below-five-second records only receive a metadata edge from an
            // earlier input record at/above five. Eligible high records are all
            // within [5, 5.5), so the earliest eligible input is a sufficient edge.
            size_t nextHigh = high, earliest = candidates.size();
            for (size_t i = begin; i < high; ++i) {
                while (nextHigh < end && !(itemFor(known[nextHigh]).durationSec - itemFor(known[i]).durationSec > 0.5)) {
                    earliest = (std::min)(earliest, known[nextHigh]); ++nextHigh;
                }
                if (earliest < known[i]) unite(earliest, known[i]);
            }
            begin = end;
        }
        if (needsText) {
            std::vector<TextKeys> keys; keys.reserve(last - first);
            for (size_t i = first; i < last; ++i) keys.push_back(textKeysFor(itemFor(ordered[i])));
            std::vector<size_t> keyed, compatible;
            keyed.reserve(keys.size()); compatible.reserve(keys.size());
            auto candidateAt = [&](size_t local) { return ordered[first + local]; };
            // A text equality is sufficient only within a compatible resolution.
            // Unknown-resolution records also connect to known records. Nearest
            // unknown neighbors on either side suffice: a one-second window can
            // meet at most two components separated by gaps greater than 0.5.
            for (int field = 0; field < 3; ++field) {
                auto textAt = [&](size_t local) -> const std::wstring& {
                    return field == 0 ? keys[local].stem : (field == 1 ? keys[local].title : keys[local].file);
                };
                keyed.clear();
                for (size_t i = 0; i < keys.size(); ++i) if (!textAt(i).empty()) keyed.push_back(i);
                std::sort(keyed.begin(), keyed.end(), [&](size_t a, size_t b) { return textAt(a) != textAt(b) ? textAt(a) < textAt(b) : a < b; });
                for (size_t begin = 0; begin < keyed.size();) {
                    size_t end = begin + 1;
                    while (end < keyed.size() && textAt(keyed[end]) == textAt(keyed[begin])) ++end;
                    if (end - begin > 1) {
                        compatible.assign(keyed.begin() + begin, keyed.begin() + end);
                        std::sort(compatible.begin(), compatible.end(), [&](size_t a, size_t b) { return resolutionThenDuration(candidateAt(a), candidateAt(b)); });
                        size_t unknownEnd = 0;
                        while (unknownEnd < compatible.size() && !knownResolution(itemFor(candidateAt(compatible[unknownEnd])))) ++unknownEnd;
                        for (size_t i = 1; i < compatible.size(); ++i) {
                            const size_t a = candidateAt(compatible[i - 1]), b = candidateAt(compatible[i]);
                            if (sameResolution(a, b) && !(itemFor(b).durationSec - itemFor(a).durationSec > 0.5)) unite(a, b);
                        }
                        for (size_t i = unknownEnd; i < compatible.size() && unknownEnd > 0; ++i) {
                            const size_t candidate = candidateAt(compatible[i]);
                            const double duration = itemFor(candidate).durationSec;
                            const auto next = std::lower_bound(compatible.begin(), compatible.begin() + unknownEnd, duration,
                                [&](size_t local, double value) { return itemFor(candidateAt(local)).durationSec < value; });
                            if (next != compatible.begin() + unknownEnd && !(itemFor(candidateAt(*next)).durationSec - duration > 0.5)) unite(candidate, candidateAt(*next));
                            if (next != compatible.begin() && !(duration - itemFor(candidateAt(*(next - 1))).durationSec > 0.5)) unite(candidate, candidateAt(*(next - 1)));
                        }
                    }
                    begin = end;
                }
            }
        }
        first = last;
    }
    struct ClusterMember { size_t root, index; };
    std::vector<ClusterMember> members;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const size_t root = rootOf(i);
        if (parent[root] < -1) members.push_back({root, candidates[i]});
    }
    std::sort(members.begin(), members.end(), [](const ClusterMember& a, const ClusterMember& b) { return a.root != b.root ? a.root < b.root : a.index < b.index; });
    for (size_t first = 0; first < members.size();) {
        size_t last = first + 1;
        while (last < members.size() && members[last].root == members[first].root) ++last;
        DuplicateGroup group; group.matchKind = DuplicateMatchKind::LikelyContent;
        const auto& keeper = populateGroup(group, members.begin() + first, members.begin() + last, [](const ClusterMember& member) { return member.index; });
        group.key = L"dup_content_" + std::to_wstring(keeper.fileSizeBytes) + L"_" + std::to_wstring((uint64_t)std::round(keeper.durationSec * 1000));
        std::wstring resolution;
        if (knownResolution(keeper)) resolution = L" • " + std::to_wstring(keeper.videoWidth) + L"x" + std::to_wstring(keeper.videoHeight);
        group.subtitle = L"Likely content match (" + std::to_wstring(group.items.size()) + L" entries • " +
            FormatFileSize(group.individualSizeBytes) + L" • " + FormatDuration(group.durationSec) + resolution + L")";
        result.groups.push_back(std::move(group)); first = last;
    }
    std::sort(result.groups.begin(), result.groups.end(), [](const DuplicateGroup& a, const DuplicateGroup& b) {
        if (a.matchKind != b.matchKind) return static_cast<int>(a.matchKind) < static_cast<int>(b.matchKind);
        const int cmp = CompareStringOrdinal(a.title.c_str(), -1, b.title.c_str(), -1, TRUE);
        if (cmp == CSTR_LESS_THAN) return true;
        if (cmp == CSTR_GREATER_THAN) return false;
        if (a.key != b.key) return a.key < b.key;
        return a.items.front()->filePath < b.items.front()->filePath;
    });
    result.totalDuplicateGroups = result.groups.size();
    for (const auto& group : result.groups) {
        result.totalDuplicateEntries += group.items.size() - 1;
        result.totalReclaimableBytes += group.totalDuplicateSizeBytes;
    }
    return result;
}

DuplicateDetectionResult LibraryQueryEngine::DetectDuplicates(const MediaLibrary& library) { return DetectDuplicates(library.mediaItems); }

// ============================================================================
// Conservative Next-Episode Detection Implementation
// ============================================================================

std::optional<EpisodeInfo> LibraryQueryEngine::FindNextEpisode( const std::wstring& currentFilePath,
    const std::vector<std::wstring>& candidatePaths, std::wstring* outAmbiguityDetails) {
    if (outAmbiguityDetails) outAmbiguityDetails->clear();
    EpisodeInfo current = ParseEpisodeInfo(currentFilePath);
    if (!current.isValid || !current.hasEpisode) { return std::nullopt; }
    int targetSeason = current.seasonNumber, targetEpisode = current.episodeNumber + 1;
    std::vector<EpisodeInfo> matches;
    for (const auto& candidatePath : candidatePaths) {
        if (candidatePath.empty()) continue;
        // 1. Same Directory check (strict cross-directory rejection)
        if (!AreSameDirectory(currentFilePath, candidatePath)) { continue; }
        // 2. Reject self
        if (CompareStringOrdinal(currentFilePath.c_str(), -1, candidatePath.c_str(), -1, TRUE) == CSTR_EQUAL) { continue; }
        // 3. Must be a supported media extension
        if (!MediaLibrary::IsSupportedMediaExtension(candidatePath)) { continue; }
        // 4. Parse episode info
        EpisodeInfo candidate = ParseEpisodeInfo(candidatePath);
        if (!candidate.isValid || !candidate.hasEpisode) { continue; }
        // 5. Must match series stem
        if (candidate.seriesStem != current.seriesStem) { continue; }
        // 6. Must match target season and target episode
        if (candidate.seasonNumber == targetSeason && candidate.episodeNumber == targetEpisode) { matches.push_back(candidate); }
    }
    if (matches.empty()) { return std::nullopt; }
    if (matches.size() > 1) {
        // Ambiguity detected!
        if (outAmbiguityDetails) {
            std::wstringstream ss;
            ss << L"Ambiguous next episode match: found " << matches.size() << L" candidate files for S"
               << targetSeason << L"E" << targetEpisode << L":\n";
            for (const auto& m : matches) { ss << L"  - " << m.rawFileName << L"\n"; }
            *outAmbiguityDetails = ss.str();
        }
        return std::nullopt;
    }
    return matches.front();
}

std::optional<EpisodeInfo> LibraryQueryEngine::FindNextEpisode( const std::wstring& currentFilePath, const MediaLibrary& library,
    std::wstring* outAmbiguityDetails) {
    std::vector<std::wstring> candidates;
    candidates.reserve(library.mediaItems.size());
    for (const auto& item : library.mediaItems) { candidates.push_back(item.filePath); }
    return FindNextEpisode(currentFilePath, candidates, outAmbiguityDetails);
}

NextEpisodeSafeAction LibraryQueryEngine::ProjectNextEpisodeAction( const std::wstring& currentFilePath,
    const std::vector<std::wstring>& candidates, const Settings& settings, const std::function<bool(const std::wstring&)>& fileExistsFn) {
    NextEpisodeSafeAction action;
    action.currentFilePath = currentFilePath;
    action.isAutoPlayAllowed = settings.autoplay;
    EpisodeInfo current = ParseEpisodeInfo(currentFilePath);
    action.currentEpisode = current;
    if (!current.isValid || !current.hasEpisode) {
        action.actionType = NextEpisodeActionType::NoNextEpisode;
        action.reason = L"Current media is not a recognized episode in a sequence";
        return action;
    }
    std::wstring ambiguityDetails;
    auto nextOpt = FindNextEpisode(currentFilePath, candidates, &ambiguityDetails);
    if (!ambiguityDetails.empty()) {
        action.actionType = NextEpisodeActionType::Ambiguous;
        action.reason = ambiguityDetails;
        action.canExecuteExplicitly = false;
        action.canExecuteAuto = false;
        return action;
    }
    if (!nextOpt.has_value()) {
        // Check if candidates in same directory have gaps or cross-directory files
        bool hasGap = false, hasCrossDir = false;
        for (const auto& path : candidates) {
            if (path == currentFilePath) continue;
            EpisodeInfo ep = ParseEpisodeInfo(path);
            if (ep.isValid && ep.seriesStem == current.seriesStem) {
                if (!AreSameDirectory(currentFilePath, path)) {
                    hasCrossDir = true;
                } else if (ep.seasonNumber == current.seasonNumber && ep.episodeNumber > current.episodeNumber + 1) { hasGap = true; }
            }
        }
        if (hasGap) {
            action.actionType = NextEpisodeActionType::GapDetected;
            action.reason = L"Next episode missing in sequence (gap detected)";
        } else if (hasCrossDir) {
            action.actionType = NextEpisodeActionType::CrossDirectoryRejected;
            action.reason = L"Next episode candidate exists only in a different directory";
        } else { action.actionType = NextEpisodeActionType::NoNextEpisode; action.reason = L"No next episode found in directory"; }
        return action;
    }
    action.nextEpisode = *nextOpt;
    action.nextFilePath = nextOpt->rawPath;
    action.nextTitle = nextOpt->rawFileName;
    // Check disk existence
    bool exists = fileExistsFn ? fileExistsFn(action.nextFilePath) : FileExists(action.nextFilePath);
    if (!exists) {
        action.actionType = NextEpisodeActionType::Disabled;
        action.reason = L"Next episode file does not exist on disk";
        action.canExecuteExplicitly = false;
        action.canExecuteAuto = false;
        return action;
    }
    action.actionType = NextEpisodeActionType::PlayNextEpisode;
    action.canExecuteExplicitly = true;
    action.canExecuteAuto = settings.autoplay;
    action.reason = settings.autoplay ? L"Next episode ready for autoplay" : L"Next episode ready (autoplay disabled)";
    return action;
}

NextEpisodeSafeAction LibraryQueryEngine::ProjectNextEpisodeAction( const std::wstring& currentFilePath, const MediaLibrary& library,
    const Settings& settings, const std::function<bool(const std::wstring&)>& fileExistsFn) {
    std::vector<std::wstring> candidates;
    candidates.reserve(library.mediaItems.size());
    for (const auto& item : library.mediaItems) { candidates.push_back(item.filePath); }
    return ProjectNextEpisodeAction(currentFilePath, candidates, settings, fileExistsFn);
}

// ============================================================================
// UI Display Helpers
// ============================================================================

std::wstring GetGroupModeDisplayName(LibraryGroupMode mode) {
    switch (mode) {
        case LibraryGroupMode::Flat: return L"Standalone";
        case LibraryGroupMode::Folder: return L"Folders";
        case LibraryGroupMode::Series: return L"Series";
        case LibraryGroupMode::Duplicates: return L"Duplicates";
    }
    return L"Standalone";
}

} // namespace Aperture
