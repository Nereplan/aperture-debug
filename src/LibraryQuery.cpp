#include "LibraryQuery.h"
#include "MediaLibrary.h"
#include "Settings.h"
#include "Common.h"

#include <algorithm>
#include <cwctype>
#include <sstream>
#include <iomanip>
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
std::wstring ExtractFileNameWithoutExt(const std::wstring& path) {
    std::wstring fileName = GetFileNameFromPath(path);
    size_t lastDot = fileName.find_last_of(L'.');
    if (lastDot != std::wstring::npos && lastDot > 0) { return fileName.substr(0, lastDot); }
    return fileName;
}

// Strips brackets like [1080p], (2024), {Erai-raws}, etc.
std::wstring StripBracketedContent(const std::wstring& text) {
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
    wchar_t inlineBuf[kInlineChars] = {};
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

void NormalizeSearchTextInto(const std::wstring& text, SearchTextBuffer& out) {
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
} // namespace

std::wstring LibraryQueryEngine::NormalizeSearchText(const std::wstring& text) {
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

std::wstring LibraryQueryEngine::ExtractSeriesStem(const std::wstring& filename) {
    std::wstring clean = StripBracketedContent(filename);
    std::wstring lower = ToLowerString(clean);
    // Replace non-alphanumeric separators with spaces
    std::wstring result;
    result.reserve(lower.size());
    bool prevSpace = false;
    for (wchar_t c : lower) {
        if (std::iswalnum(c)) { result.push_back(c); prevSpace = false; } else {
            if (!prevSpace && !result.empty()) { result.push_back(L' '); prevSpace = true; }
        }
    }
    while (!result.empty() && result.back() == L' ') { result.pop_back(); }
    return result;
}

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

EpisodeInfo LibraryQueryEngine::ParseEpisodeInfo(const std::wstring& filePath) {
    EpisodeInfo info;
    info.rawPath = filePath;
    info.rawFileName = GetFileNameFromPath(filePath);
    info.directoryPath = ExtractDirectory(filePath);
    info.isValid = false;
    // Reject non-supported media extensions
    if (!MediaLibrary::IsSupportedMediaExtension(filePath)) { return info; }
    std::wstring baseName = ExtractFileNameWithoutExt(info.rawFileName);
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
        std::wstring rawStem = cleanName.substr(0, stemEndPos);
        info.seriesStem = ExtractSeriesStem(rawStem);
        if (info.seriesStem.empty()) {
            // Fallback to parent directory stem if filename stem was empty
            info.seriesStem = ExtractSeriesStem(GetFileNameFromPath(info.directoryPath));
        }
        info.isValid = !info.seriesStem.empty();
    }
    return info;
}

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
    auto containsNormalized = [&](const std::wstring& text) {
        NormalizeSearchTextInto(text, buffer);
        return buffer.View().find(normalizedQuery) != std::wstring_view::npos;
    };
    if (containsNormalized(item.GetDisplayTitle())) return true;
    if (containsNormalized(item.title)) return true;
    if (containsNormalized(GetFileNameFromPath(item.filePath))) return true;
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
    return cachedLibraryVersion == library.GetVersion() && cachedProjection.options == options &&
           cachedLibraryVersion != (std::numeric_limits<uint64_t>::max)();
}

void LibraryQueryEngine::InvalidateCache() {
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
    return true;
}

const LibraryQueryProjection& LibraryQueryEngine::Query( const MediaLibrary& library, const LibraryQueryOptions& options,
    const std::function<bool(const std::wstring&)>& fileExistsFn) {
    if (IsCacheValid(library, options)) { return cachedProjection; }
    LibraryQueryProjection projection;
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
    auto projectItem = [](const ItemMatch& match) {
        const auto& item = *match.item;
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
        projected.existsOnDisk = match.existsOnDisk;
        projected.originalLibraryIndex = match.originalIndex;
        return projected;
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
            const EpisodeInfo episode = ParseEpisodeInfo(item.filePath);
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
            grp.startIndex = projection.items.size();
            grp.count = 0;
            grp.isCollapsed = options.CollapsedGroupCount(dgrp.key);
            for (const auto& m : groupMatches) {
                const auto& item = *m.item;
                grp.totalSizeBytes += item.fileSizeBytes;
                grp.totalDurationSec += item.durationSec;
                grp.count++;
                if (!grp.isCollapsed) {
                    if (options.maxItems > 0 && projection.items.size() >= options.maxItems) { projection.isCapped = true; break; }
                    projection.items.push_back(projectItem(m));
                }
            }
            if (grp.count > 0) { projection.groups.push_back(std::move(grp)); }
            if (options.maxItems > 0 && projection.items.size() >= options.maxItems) { break; }
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
                return CompareItems(*a.item, *b.item, options.sortField, options.sortDirection) < 0;
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
        projection.items.reserve(countToProject);
        for (size_t i = 0; i < countToProject; ++i) { const auto& m = matches[i]; projection.items.push_back(projectItem(m)); }
    } else {
        // Grouped Mode (Folder or Series)
        if (options.groupMode == LibraryGroupMode::Folder && !options.customFolderOrder.empty()) {
            std::unordered_map<std::wstring, size_t> folderOrder;
            folderOrder.reserve(options.customFolderOrder.size());
            for (size_t i = 0; i < options.customFolderOrder.size(); ++i) {
                // The first occurrence wins, matching the old linear lookup.
                folderOrder.emplace(NormalizeDirectoryPath(options.customFolderOrder[i]), i);
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
        // Sort items inside each group
        for (auto& bucket : buckets) {
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
                    return CompareItems(*a.item, *b.item, options.sortField, options.sortDirection) < 0;
                });
            } else {
                // Folder mode or standalone series group: sort by active sortField & sortDirection
                std::sort(bucket.items.begin(), bucket.items.end(), [&](const ItemMatch& a, const ItemMatch& b) {
                    return CompareItems(*a.item, *b.item, options.sortField, options.sortDirection) < 0;
                });
            }
        }
        // Flatten groups into projection.items and record groups
        projection.groups.reserve(buckets.size());
        size_t visibleCount = 0;
        for (const auto& bucket : buckets) {
            if (!options.CollapsedGroupCount(bucket.key)) visibleCount += bucket.items.size();
        }
        projection.items.reserve(options.maxItems > 0 ? (std::min)(visibleCount, options.maxItems) : visibleCount);
        for (const auto& bucket : buckets) {
            if (bucket.items.empty()) continue;
            LibraryGroupSummary grp;
            grp.key = bucket.key;
            grp.title = bucket.title;
            grp.subtitle = bucket.subtitle;
            grp.isSpecial = bucket.isSpecial;
            grp.startIndex = projection.items.size();
            grp.count = bucket.items.size();
            grp.isCollapsed = options.CollapsedGroupCount(bucket.key);
            for (const auto& m : bucket.items) {
                const auto& item = *m.item;
                grp.totalSizeBytes += item.fileSizeBytes;
                grp.totalDurationSec += item.durationSec;
                if (!grp.isCollapsed) {
                    if (options.maxItems > 0 && projection.items.size() >= options.maxItems) { projection.isCapped = true; break; }
                    projection.items.push_back(projectItem(m));
                }
            }
            projection.groups.push_back(std::move(grp));
            if (options.maxItems > 0 && projection.items.size() >= options.maxItems) { break; }
        }
    }
    // Cache the projection: move into the engine-owned slot and return it by reference so callers pay no copy on hit or miss.
    cachedProjection = std::move(projection);
    cachedLibraryVersion = library.GetVersion();
    return cachedProjection;
}

// ============================================================================
// Conservative Duplicate Detection Implementation
// ============================================================================

DuplicateDetectionResult LibraryQueryEngine::DetectDuplicates(const std::vector<MediaItem>& items) {
    DuplicateDetectionResult result;
    const size_t N = items.size();
    if (N < 2) return result;
    std::vector<bool> assigned(N, false);
    // --- Pass 1A: Exact Windows File Identity (volumeSerialNumber + fileIndex) ---
    struct VolIdxKey {
        uint32_t vol;
        uint64_t idx;
        bool operator==(const VolIdxKey& o) const { return vol == o.vol && idx == o.idx; }
    };
    struct VolIdxHash {
        size_t operator()(const VolIdxKey& k) const { return std::hash<uint64_t>()(((uint64_t)k.vol << 32) ^ k.idx); }
    };
    std::unordered_map<VolIdxKey, std::vector<size_t>, VolIdxHash> volIdxGroups;
    for (size_t i = 0; i < N; ++i) {
        const auto& item = items[i];
        if (item.volumeSerialNumber != 0 && item.fileIndex != 0) { volIdxGroups[{item.volumeSerialNumber, item.fileIndex}].push_back(i); }
    }
    for (const auto& pair : volIdxGroups) {
        const auto& key = pair.first;
        const auto& indices = pair.second;
        if (indices.size() >= 2) {
            DuplicateGroup grp;
            grp.matchKind = DuplicateMatchKind::ExactFileIdentity;
            grp.key = L"dup_exact_fid_" + std::to_wstring(key.vol) + L"_" + std::to_wstring(key.idx);
            for (size_t idx : indices) { assigned[idx] = true; grp.items.push_back(&items[idx]); grp.originalIndices.push_back(idx); }
            // Deterministic sort within group (keeper first)
            std::vector<size_t> perm(grp.items.size());
            std::iota(perm.begin(), perm.end(), 0);
            std::sort(perm.begin(), perm.end(), [&](size_t a, size_t b) {
                const auto& itemA = *grp.items[a];
                const auto& itemB = *grp.items[b];
                bool playedA = (itemA.status == "done" || itemA.lastPosSec > 0.0);
                bool playedB = (itemB.status == "done" || itemB.lastPosSec > 0.0);
                if (playedA != playedB) return playedA > playedB;
                if (itemA.GetProgressRatio() != itemB.GetProgressRatio()) { return itemA.GetProgressRatio() > itemB.GetProgressRatio(); }
                if (itemA.addedTimestamp != itemB.addedTimestamp && itemA.addedTimestamp > 0 && itemB.addedTimestamp > 0)
                    return itemA.addedTimestamp < itemB.addedTimestamp;
                if (itemA.filePath.size() != itemB.filePath.size()) { return itemA.filePath.size() < itemB.filePath.size(); }
                int cmp = CompareStringOrdinal(itemA.filePath.c_str(), -1, itemB.filePath.c_str(), -1, TRUE);
                if (cmp == CSTR_LESS_THAN) return true;
                if (cmp == CSTR_GREATER_THAN) return false;
                return itemA.id < itemB.id;
            });
            std::vector<const MediaItem*> sortedItems;
            std::vector<size_t> sortedIndices;
            for (size_t p : perm) { sortedItems.push_back(grp.items[p]); sortedIndices.push_back(grp.originalIndices[p]); }
            grp.items = std::move(sortedItems);
            grp.originalIndices = std::move(sortedIndices);
            grp.suggestedKeepIndex = 0;
            const auto& keeper = *grp.items[0];
            grp.title = keeper.title.empty() ? GetFileNameFromPath(keeper.filePath) : keeper.title;
            grp.individualSizeBytes = keeper.fileSizeBytes;
            grp.totalDuplicateSizeBytes = (grp.items.size() - 1) * grp.individualSizeBytes;
            grp.durationSec = keeper.durationSec;
            grp.subtitle = L"Exact file identity (" + std::to_wstring(grp.items.size()) + L" entries \x2022 matching volume & file index)";
            result.groups.push_back(std::move(grp));
        }
    }
    // --- Pass 1B: Exact Normalized Path Alias ---
    std::unordered_map<std::wstring, std::vector<size_t>> pathAliasGroups;
    for (size_t i = 0; i < N; ++i) {
        if (assigned[i]) continue;
        const auto& item = items[i];
        std::wstring normPath = NormalizePathKey(item.filePath);
        if (!normPath.empty()) { pathAliasGroups[normPath].push_back(i); }
    }
    for (const auto& pair : pathAliasGroups) {
        const auto& normPath = pair.first;
        const auto& indices = pair.second;
        if (indices.size() >= 2) {
            DuplicateGroup grp;
            grp.matchKind = DuplicateMatchKind::ExactPathAlias;
            grp.key = L"dup_exact_path_" + normPath;
            for (size_t idx : indices) { assigned[idx] = true; grp.items.push_back(&items[idx]); grp.originalIndices.push_back(idx); }
            std::vector<size_t> perm(grp.items.size());
            std::iota(perm.begin(), perm.end(), 0);
            std::sort(perm.begin(), perm.end(), [&](size_t a, size_t b) {
                const auto& itemA = *grp.items[a];
                const auto& itemB = *grp.items[b];
                bool playedA = (itemA.status == "done" || itemA.lastPosSec > 0.0);
                bool playedB = (itemB.status == "done" || itemB.lastPosSec > 0.0);
                if (playedA != playedB) return playedA > playedB;
                if (itemA.GetProgressRatio() != itemB.GetProgressRatio()) { return itemA.GetProgressRatio() > itemB.GetProgressRatio(); }
                if (itemA.addedTimestamp != itemB.addedTimestamp && itemA.addedTimestamp > 0 && itemB.addedTimestamp > 0)
                    return itemA.addedTimestamp < itemB.addedTimestamp;
                if (itemA.filePath.size() != itemB.filePath.size()) { return itemA.filePath.size() < itemB.filePath.size(); }
                int cmp = CompareStringOrdinal(itemA.filePath.c_str(), -1, itemB.filePath.c_str(), -1, TRUE);
                if (cmp == CSTR_LESS_THAN) return true;
                if (cmp == CSTR_GREATER_THAN) return false;
                return itemA.id < itemB.id;
            });
            std::vector<const MediaItem*> sortedItems;
            std::vector<size_t> sortedIndices;
            for (size_t p : perm) { sortedItems.push_back(grp.items[p]); sortedIndices.push_back(grp.originalIndices[p]); }
            grp.items = std::move(sortedItems);
            grp.originalIndices = std::move(sortedIndices);
            grp.suggestedKeepIndex = 0;
            const auto& keeper = *grp.items[0];
            grp.title = keeper.title.empty() ? GetFileNameFromPath(keeper.filePath) : keeper.title;
            grp.individualSizeBytes = keeper.fileSizeBytes;
            grp.totalDuplicateSizeBytes = (grp.items.size() - 1) * grp.individualSizeBytes;
            grp.durationSec = keeper.durationSec;
            grp.subtitle = L"Exact path alias (" + std::to_wstring(grp.items.size()) + L" entries \x2022 same normalized location)";
            result.groups.push_back(std::move(grp));
        }
    }
    // --- Pass 2: Conservative Content Match ---
    std::vector<size_t> candidateIndices;
    for (size_t i = 0; i < N; ++i) {
        if (assigned[i]) continue;
        const auto& item = items[i];
        if (item.fileSizeBytes > 0 && item.durationSec > 0.5) { candidateIndices.push_back(i); }
    }
    struct ContentTextKeys { std::wstring stem; std::wstring title; std::wstring file; };
    // Populate only after cheap metadata checks pass; unrelated sizes and
    // durations never need text normalization. Reuse keys across comparisons.
    std::vector<std::optional<ContentTextKeys>> contentTextKeys(candidateIndices.size());
    auto textKeysFor = [&](size_t candidate) -> const ContentTextKeys& {
        auto& cached = contentTextKeys[candidate];
        if (!cached) {
            const auto& item = items[candidateIndices[candidate]];
            const auto fileName = GetFileNameFromPath(item.filePath);
            cached = ContentTextKeys{ExtractSeriesStem(fileName), NormalizeSearchText(item.title.empty() ? fileName : item.title),
                                     NormalizeSearchText(ExtractFileNameWithoutExt(item.filePath))};
        }
        return *cached;
    };
    auto MatchesContentConservatively = [&](size_t first, size_t second) -> bool {
        const auto& a = items[candidateIndices[first]];
        const auto& b = items[candidateIndices[second]];
        if (a.fileSizeBytes == 0 || b.fileSizeBytes == 0) return false;
        if (a.fileSizeBytes != b.fileSizeBytes) return false;
        if (a.durationSec <= 0.5 || b.durationSec <= 0.5) return false;
        if (std::abs(a.durationSec - b.durationSec) > 0.5) return false;
        if (a.videoWidth > 0 && a.videoHeight > 0 && b.videoWidth > 0 && b.videoHeight > 0) {
            if (a.videoWidth != b.videoWidth || a.videoHeight != b.videoHeight) return false;
        }
        const auto& keysA = textKeysFor(first);
        const auto& keysB = textKeysFor(second);
        if (!keysA.stem.empty() && keysA.stem == keysB.stem) return true;
        if (!keysA.title.empty() && keysA.title == keysB.title) return true;
        if (!keysA.file.empty() && keysA.file == keysB.file) return true;
        if (a.videoWidth > 0 && a.videoHeight > 0 && b.videoWidth > 0 && b.videoHeight > 0 &&
            a.videoWidth == b.videoWidth && a.videoHeight == b.videoHeight && a.durationSec >= 5.0) {
            return true;
        }
        return false;
    };
    const size_t numCands = candidateIndices.size();
    std::vector<int> parent(numCands);
    std::iota(parent.begin(), parent.end(), 0);
    auto FindRoot = [&](auto& self, int x) -> int { if (parent[x] == x) return x; return parent[x] = self(self, parent[x]); };
    auto UnionSets = [&](int a, int b) {
        int ra = FindRoot(FindRoot, a);
        int rb = FindRoot(FindRoot, b);
        if (ra != rb) { parent[rb] = ra; }
    };
    // Size equality is mandatory. Retain the original union order, but skip unrelated sizes instead of visiting every pair in the library.
    std::unordered_map<uint64_t, std::vector<size_t>> candidatesBySize;
    for (size_t i = 0; i < numCands; ++i) { candidatesBySize[items[candidateIndices[i]].fileSizeBytes].push_back(i); }
    for (size_t i = 0; i < numCands; ++i) {
        const auto& bucket = candidatesBySize.at(items[candidateIndices[i]].fileSizeBytes);
        for (auto next = std::upper_bound(bucket.begin(), bucket.end(), i); next != bucket.end(); ++next) {
            const size_t j = *next;
            if (MatchesContentConservatively(i, j)) { UnionSets(static_cast<int>(i), static_cast<int>(j)); }
        }
    }
    std::unordered_map<int, std::vector<size_t>> contentClusters;
    for (size_t i = 0; i < numCands; ++i) {
        int root = FindRoot(FindRoot, static_cast<int>(i));
        contentClusters[root].push_back(candidateIndices[i]);
    }
    for (const auto& pair : contentClusters) {
        const auto& clusterIndices = pair.second;
        if (clusterIndices.size() >= 2) {
            DuplicateGroup grp;
            grp.matchKind = DuplicateMatchKind::LikelyContent;
            for (size_t idx : clusterIndices) {
                assigned[idx] = true;
                grp.items.push_back(&items[idx]);
                grp.originalIndices.push_back(idx);
            }
            std::vector<size_t> perm(grp.items.size());
            std::iota(perm.begin(), perm.end(), 0);
            std::sort(perm.begin(), perm.end(), [&](size_t a, size_t b) {
                const auto& itemA = *grp.items[a];
                const auto& itemB = *grp.items[b];
                bool playedA = (itemA.status == "done" || itemA.lastPosSec > 0.0);
                bool playedB = (itemB.status == "done" || itemB.lastPosSec > 0.0);
                if (playedA != playedB) return playedA > playedB;
                if (itemA.GetProgressRatio() != itemB.GetProgressRatio()) { return itemA.GetProgressRatio() > itemB.GetProgressRatio(); }
                if (itemA.addedTimestamp != itemB.addedTimestamp && itemA.addedTimestamp > 0 && itemB.addedTimestamp > 0)
                    return itemA.addedTimestamp < itemB.addedTimestamp;
                if (itemA.filePath.size() != itemB.filePath.size()) { return itemA.filePath.size() < itemB.filePath.size(); }
                int cmp = CompareStringOrdinal(itemA.filePath.c_str(), -1, itemB.filePath.c_str(), -1, TRUE);
                if (cmp == CSTR_LESS_THAN) return true;
                if (cmp == CSTR_GREATER_THAN) return false;
                return itemA.id < itemB.id;
            });
            std::vector<const MediaItem*> sortedItems;
            std::vector<size_t> sortedIndices;
            for (size_t p : perm) { sortedItems.push_back(grp.items[p]); sortedIndices.push_back(grp.originalIndices[p]); }
            grp.items = std::move(sortedItems);
            grp.originalIndices = std::move(sortedIndices);
            grp.suggestedKeepIndex = 0;
            const auto& keeper = *grp.items[0];
            grp.key = L"dup_content_" + std::to_wstring(keeper.fileSizeBytes) + L"_" + std::to_wstring((uint64_t)std::round(keeper.durationSec * 1000));
            grp.title = keeper.title.empty() ? GetFileNameFromPath(keeper.filePath) : keeper.title;
            grp.individualSizeBytes = keeper.fileSizeBytes;
            grp.totalDuplicateSizeBytes = (grp.items.size() - 1) * grp.individualSizeBytes;
            grp.durationSec = keeper.durationSec;
            std::wstring resStr;
            if (keeper.videoWidth > 0 && keeper.videoHeight > 0)
                resStr = L" \x2022 " + std::to_wstring(keeper.videoWidth) + L"x" + std::to_wstring(keeper.videoHeight);
            grp.subtitle = L"Likely content match (" + std::to_wstring(grp.items.size()) + L" entries \x2022 " +
                           FormatFileSize(grp.individualSizeBytes) + L" \x2022 " + FormatDuration(grp.durationSec) + resStr + L")";
            result.groups.push_back(std::move(grp));
        }
    }
    // --- Deterministic Sorting of Groups ---
    std::sort(result.groups.begin(), result.groups.end(), [](const DuplicateGroup& a, const DuplicateGroup& b) {
        if (a.matchKind != b.matchKind) { return static_cast<int>(a.matchKind) < static_cast<int>(b.matchKind); }
        int cmp = CompareStringOrdinal(a.title.c_str(), -1, b.title.c_str(), -1, TRUE);
        if (cmp == CSTR_LESS_THAN) return true;
        if (cmp == CSTR_GREATER_THAN) return false;
        return a.key < b.key;
    });
    // Compute totals
    result.totalDuplicateGroups = result.groups.size();
    result.totalDuplicateEntries = 0;
    result.totalReclaimableBytes = 0;
    for (const auto& grp : result.groups) {
        if (grp.items.size() > 1) {
            result.totalDuplicateEntries += (grp.items.size() - 1);
            result.totalReclaimableBytes += grp.totalDuplicateSizeBytes;
        }
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
