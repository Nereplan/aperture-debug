#include "GlobalSearch.h"
#include <cwctype>

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

namespace {
bool IsSearchTokenDelimiter(wchar_t c) {
    if (c > 127) return std::iswspace(c) != 0;
    switch (c) {
        case L' ': case L'\t': case L'\r': case L'\n': case L'\v': case L'\f':
        case L'.': case L'_': case L'-': case L'\\': case L'/':
        case L'[': case L']': case L'(': case L')': case L'{': case L'}':
        case L',': case L';': case L':': case L'!': case L'?': case L'+':
        case L'=': case L'\'': case L'"': case L'@': case L'#': case L'$':
        case L'%': case L'^': case L'&': case L'*': case L'|': case L'~':
        case L'`': case L'<': case L'>': return true;
        default: return false;
    }
}

std::wstring_view SearchFileNameView(std::wstring_view path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
}

size_t NormalizedFileNameOffset(const std::wstring& path) {
    auto name = SearchFileNameView(path);
    while (!name.empty() && (name.front() == L'\0' || std::iswspace(name.front()))) name.remove_prefix(1);
    // NormalizeText already trims whitespace/NULs at the end of the full path.
    return path.size() - name.size();
}

CachedSearchTokens BuildSearchTokens(const std::wstring& original, const std::wstring& normalized) {
    CachedSearchTokens result;
    if (original.find(L'\0') != std::wstring::npos) {
        result.originalText = std::make_unique<std::wstring>(original);
        for (auto& c : *result.originalText) c = static_cast<wchar_t>(std::towlower(c));
    }
    const auto& text = result.originalText ? *result.originalText : normalized;
    size_t count = 0;
    bool inToken = false;
    for (const wchar_t c : text) {
        const bool delimiter = IsSearchTokenDelimiter(c);
        if (!delimiter && !inToken) ++count;
        inToken = !delimiter;
    }
    result.ranges.reserve(count);
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || IsSearchTokenDelimiter(text[i])) {
            if (i > start) result.ranges.push_back({start, i - start});
            start = i + 1;
        }
    }
    return result;
}
} // namespace

std::wstring_view CachedSearchEntry::GetFileNameView() const { return SearchFileNameView(filePath); }
std::wstring_view CachedSearchEntry::GetNormalizedFileNameView() const {
    return {normalizedPath.data() + normalizedFileNameOffset, normalizedPath.size() - normalizedFileNameOffset};
}

std::vector<std::wstring> GlobalSearchIndex::Tokenize(const std::wstring& text) {
    std::vector<std::wstring> tokens;
    if (text.empty()) return tokens;
    std::wstring current;
    for (wchar_t c : text) {
        if (IsSearchTokenDelimiter(c)) {
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

bool GlobalSearchIndex::IsDirty(const MediaLibrary& library) const {
    return library.GetSearchContentVersion() != cachedSearchContentVersion || !cachedRevisions.SameSearchData(library.GetRevisions());
}

void GlobalSearchIndex::Invalidate() {
    cachedSearchContentVersion = (std::numeric_limits<uint64_t>::max)();
    entries.clear(); historyEntryByPath.clear(); historyMetadataByEntry.clear();
    candidateBuckets.clear(); candidatePostings.clear(); candidateIndexAttempted = false;
}

void GlobalSearchIndex::RebuildIndex(const MediaLibrary& library) {
    entries.clear();
    historyEntryByPath.clear();
    historyMetadataByEntry.clear();
    candidateBuckets.clear(); candidatePostings.clear(); candidateIndexAttempted = false;
    ++metadataGeneration;
    historyEntryByPath.reserve(library.history.size());
    ++textRebuildCount;
    std::unordered_map<std::wstring, size_t> pathToEntryIndex;
    const size_t entryCapacity = library.mediaItems.size() + library.history.size();
    entries.reserve(entryCapacity);
    const size_t missingEntry = (std::numeric_limits<size_t>::max)();
    pathToEntryIndex.reserve(library.history.size());
    for (const auto& hist : library.history) {
        auto key = MediaLibrary::NormalizePathKey(hist.filePath);
        if (!key.empty()) pathToEntryIndex.emplace(std::move(key), missingEntry);
    }
    // 1. Process Media Items from MediaLibrary
    for (const auto& item : library.mediaItems) {
        if (item.filePath.empty()) continue;
        if (!pathToEntryIndex.empty()) {
            const auto key = MediaLibrary::NormalizePathKey(item.filePath);
            if (key.empty()) continue;
            const auto history = pathToEntryIndex.find(key);
            if (history != pathToEntryIndex.end()) history->second = entries.size();
        }
        CachedSearchEntry entry;
        entry.metadataGeneration = metadataGeneration;
        entry.mediaId = item.id;
        entry.filePath = item.filePath;
        entry.title = item.GetDisplayTitle();
        entry.normalizedTitle = NormalizeText(entry.title);
        entry.normalizedPath = NormalizeText(item.filePath);
        entry.normalizedFileNameOffset = NormalizedFileNameOffset(entry.normalizedPath);
        entry.titleTokens = BuildSearchTokens(entry.title, entry.normalizedTitle);
        entry.pathTokens = BuildSearchTokens(item.filePath, entry.normalizedPath);
        entry.durationSec = item.durationSec;
        entry.lastPosSec = item.lastPosSec;
        entry.lastWatchedTimestamp = item.lastWatchedTimestamp;
        entry.addedTimestamp = item.addedTimestamp;
        entry.source = GlobalSearchResultSource::Media;
        entries.push_back(std::move(entry));
    }
    // 2. Process History Records from MediaLibrary
    for (const auto& hist : library.history) {
        std::wstring normKey = MediaLibrary::NormalizePathKey(hist.filePath);
        if (normKey.empty()) continue;
        auto it = pathToEntryIndex.find(normKey);
        if (it != pathToEntryIndex.end() && it->second != missingEntry) {
            // Already present in library, elevate source and merge latest stats
            CachedSearchEntry& existing = entries[it->second];
            historyEntryByPath[normKey] = it->second;
            existing.source = GlobalSearchResultSource::MediaAndHistory;
            if (hist.watchedTimestamp > existing.lastWatchedTimestamp) {
                existing.lastWatchedTimestamp = hist.watchedTimestamp;
                existing.lastPosSec = hist.lastPosSec;
            }
            if (existing.title.empty() && !hist.title.empty()) {
                existing.title = hist.title;
                existing.normalizedTitle = NormalizeText(hist.title);
                existing.titleTokens = BuildSearchTokens(hist.title, existing.normalizedTitle);
            }
            if (existing.durationSec <= 0.0 && hist.durationSec > 0.0) { existing.durationSec = hist.durationSec; }
        } else {
            // History only entry
            CachedSearchEntry entry;
            entry.metadataGeneration = metadataGeneration;
            entry.filePath = hist.filePath;
            entry.title = hist.title.empty() ? ExtractFileName(hist.filePath) : hist.title;
            entry.normalizedTitle = NormalizeText(entry.title);
            entry.normalizedPath = NormalizeText(hist.filePath);
            entry.normalizedFileNameOffset = NormalizedFileNameOffset(entry.normalizedPath);
            entry.titleTokens = BuildSearchTokens(entry.title, entry.normalizedTitle);
            entry.pathTokens = BuildSearchTokens(hist.filePath, entry.normalizedPath);
            entry.durationSec = hist.durationSec;
            entry.lastPosSec = hist.lastPosSec;
            entry.lastWatchedTimestamp = hist.watchedTimestamp;
            entry.addedTimestamp = hist.watchedTimestamp;
            entry.source = GlobalSearchResultSource::History;
            pathToEntryIndex[normKey] = entries.size();
            historyEntryByPath[normKey] = entries.size();
            entries.push_back(std::move(entry));
        }
    }
    cachedRevisions = library.GetRevisions();
    cachedSearchContentVersion = library.GetSearchContentVersion();
}

void GlobalSearchIndex::RefreshMetadata(const MediaLibrary& library) {
    // History is bounded independently of the catalog. Prepare its overlays
    // once per revision; media details are loaded only for scored matches.
    historyMetadataByEntry.clear();
    for (const auto& history : library.history) {
        const auto it = historyEntryByPath.find(MediaLibrary::NormalizePathKey(history.filePath));
        if (it == historyEntryByPath.end()) continue;
        historyMetadataByEntry[it->second].push_back({history.durationSec, history.lastPosSec, history.watchedTimestamp});
    }
    ++metadataGeneration;
    cachedRevisions = library.GetRevisions();
    cachedSearchContentVersion = library.GetSearchContentVersion();
    ++metadataRefreshCount;
}

void GlobalSearchIndex::RefreshEntryMetadata(CachedSearchEntry& entry, size_t index, const MediaLibrary& library) {
    if (entry.metadataGeneration == metadataGeneration) return;
    if (entry.source == GlobalSearchResultSource::History) {
        entry.durationSec = entry.lastPosSec = 0;
        entry.lastWatchedTimestamp = entry.addedTimestamp = 0;
    } else {
        const auto* media = entry.mediaId.empty() ? library.FindByPath(entry.filePath) : library.FindById(entry.mediaId);
        if (media) {
            entry.durationSec = media->durationSec; entry.lastPosSec = media->lastPosSec;
            entry.lastWatchedTimestamp = media->lastWatchedTimestamp; entry.addedTimestamp = media->addedTimestamp;
        }
    }
    const auto overlays = historyMetadataByEntry.find(index);
    if (overlays != historyMetadataByEntry.end()) for (const auto& history : overlays->second) {
        if (entry.source == GlobalSearchResultSource::History || history.watchedTimestamp > entry.lastWatchedTimestamp) {
            entry.lastPosSec = history.lastPosSec; entry.lastWatchedTimestamp = history.watchedTimestamp;
        }
        if (entry.source == GlobalSearchResultSource::History) {
            entry.durationSec = history.durationSec; entry.addedTimestamp = history.watchedTimestamp;
        } else if (entry.durationSec <= 0 && history.durationSec > 0) entry.durationSec = history.durationSec;
    }
    entry.metadataGeneration = metadataGeneration;
    ++metadataEntryRefreshCount;
}

namespace {
constexpr size_t kSearchCandidateBucketCount = 65536;
constexpr uint32_t kIgnoredSearchBucket = (std::numeric_limits<uint32_t>::max)();
constexpr size_t kSearchPostingBudget = 4 * 1024 * 1024;
constexpr size_t kSearchTextBuildBudget = 16 * 1024 * 1024;
constexpr uint32_t kSearchPostingLengthLimit = 1024;

uint32_t SearchTrigramBucket(wchar_t a, wchar_t b, wchar_t c) {
    uint32_t hash = (static_cast<uint32_t>(a) * 16777619u) ^ static_cast<uint32_t>(b);
    hash = (hash * 16777619u) ^ static_cast<uint32_t>(c);
    hash ^= hash >> 16;
    return hash & (kSearchCandidateBucketCount - 1);
}

template <typename Visitor> void VisitSearchTrigrams(const CachedSearchEntry& entry, Visitor visit) {
    const auto text = [&](const std::wstring& value) {
        for (size_t i = 2; i < value.size(); ++i) visit(SearchTrigramBucket(value[i - 2], value[i - 1], value[i]));
    };
    text(entry.normalizedTitle); text(entry.normalizedPath);
    if (entry.titleTokens.originalText) text(*entry.titleTokens.originalText);
    if (entry.pathTokens.originalText) text(*entry.pathTokens.originalText);
}
}

void GlobalSearchIndex::BuildCandidateIndex() {
    candidateIndexAttempted = true;
    if (entries.size() < 256 || entries.size() >= kIgnoredSearchBucket) return;
    // Fixed tables and short posting lists put a hard bound on retained
    // memory. Oversized catalogs use the existing exhaustive scorer instead.
    size_t textSize = 0;
    for (const auto& entry : entries) {
        const size_t length = entry.normalizedTitle.size() + entry.normalizedPath.size() +
            (entry.titleTokens.originalText ? entry.titleTokens.originalText->size() : 0) +
            (entry.pathTokens.originalText ? entry.pathTokens.originalText->size() : 0);
        if (length > kSearchTextBuildBudget - textSize) return;
        textSize += length;
    }
    std::vector<uint32_t> counts(kSearchCandidateBucketCount), seen(kSearchCandidateBucketCount);
    for (size_t i = 0; i < entries.size(); ++i) {
        const uint32_t stamp = static_cast<uint32_t>(i + 1);
        VisitSearchTrigrams(entries[i], [&](uint32_t bucket) {
            if (seen[bucket] == stamp) return;
            seen[bucket] = stamp;
            if (counts[bucket] <= kSearchPostingLengthLimit) ++counts[bucket];
        });
    }
    std::vector<uint32_t> useful;
    useful.reserve(kSearchCandidateBucketCount);
    for (uint32_t bucket = 0; bucket < kSearchCandidateBucketCount; ++bucket)
        if (counts[bucket] > 0 && counts[bucket] <= kSearchPostingLengthLimit) useful.push_back(bucket);
    std::sort(useful.begin(), useful.end(), [&](uint32_t a, uint32_t b) {
        return counts[a] != counts[b] ? counts[a] < counts[b] : a < b;
    });
    candidateBuckets.resize(kSearchCandidateBucketCount);
    for (size_t i = 0; i < counts.size(); ++i)
        if (counts[i] > 0) candidateBuckets[i].count = kIgnoredSearchBucket;
    size_t postingCount = 0;
    for (const uint32_t bucket : useful) {
        if (counts[bucket] > kSearchPostingBudget - postingCount) break;
        candidateBuckets[bucket] = {static_cast<uint32_t>(postingCount), counts[bucket]};
        postingCount += counts[bucket];
    }
    candidatePostings.resize(postingCount);
    std::fill(counts.begin(), counts.end(), 0);
    std::fill(seen.begin(), seen.end(), 0);
    for (size_t i = 0; i < entries.size(); ++i) {
        const uint32_t stamp = static_cast<uint32_t>(i + 1);
        VisitSearchTrigrams(entries[i], [&](uint32_t bucket) {
            if (seen[bucket] == stamp) return;
            seen[bucket] = stamp;
            const auto& posting = candidateBuckets[bucket];
            if (posting.count != kIgnoredSearchBucket && posting.count > 0)
                candidatePostings[posting.offset + counts[bucket]++] = static_cast<uint32_t>(i);
        });
    }
    ++candidateIndexBuildCount;
}

const std::pair<const uint32_t*, size_t> GlobalSearchIndex::SelectCandidates(
    const std::wstring& query, const std::vector<std::wstring>& tokens) {
    const bool hasGram = tokens.size() > 1 ? std::any_of(tokens.begin(), tokens.end(), [](const auto& token) { return token.size() >= 3; }) : query.size() >= 3;
    if (!hasGram) return {nullptr, entries.size()};
    if (!candidateIndexAttempted) BuildCandidateIndex();
    if (candidateBuckets.empty()) return {nullptr, entries.size()};
    const CandidateBucket* best = nullptr;
    const auto select = [&](const std::wstring& text) {
        for (size_t i = 2; i < text.size(); ++i) {
            const auto& posting = candidateBuckets[SearchTrigramBucket(text[i - 2], text[i - 1], text[i])];
            if (posting.count != kIgnoredSearchBucket && (!best || posting.count < best->count)) best = &posting;
        }
    };
    // Each multi-word token is required unless a title/filename phrase has
    // already matched; that phrase also contains every token's trigrams.
    if (tokens.size() > 1) { for (const auto& token : tokens) select(token); } else { select(query); }
    if (!best || best->count >= entries.size()) return {nullptr, entries.size()};
    // nullptr with zero count is an indexed, proven-empty candidate set.
    return {best->count ? candidatePostings.data() + best->offset : nullptr, best->count};
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
    } else {
        const auto normalizedFileName = entry.GetNormalizedFileNameView();
        if (normalizedFileName == normalizedQuery) {
            score += 5000;
        } else if (!normalizedFileName.empty() && normalizedFileName.rfind(normalizedQuery, 0) == 0) { score += 4000; } else {
            // Token boundary / prefix matches in title
            bool matchedTokenPrefix = false;
            for (const auto& range : entry.titleTokens.ranges) {
                const auto tok = entry.titleTokens.View(entry.normalizedTitle, range);
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
                    size_t filePos = normalizedFileName.find(normalizedQuery);
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
    }
    // 2. Multi-Token Query Refinement
    if (queryTokens.size() > 1) {
        int tokenMatchScore = 0;
        bool allTokensMatched = true;
        for (const auto& qTok : queryTokens) {
            bool tokenFound = false;
            // Check title tokens
            for (const auto& range : entry.titleTokens.ranges) {
                const auto tTok = entry.titleTokens.View(entry.normalizedTitle, range);
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
                for (const auto& range : entry.pathTokens.ranges) {
                    const auto pTok = entry.pathTokens.View(entry.normalizedPath, range);
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
    lastCandidateCount = 0;
    std::wstring normalizedQuery = NormalizeText(rawQuery);
    if (normalizedQuery.empty()) { return {}; }
    if (library.GetSearchContentVersion() != cachedSearchContentVersion) { RebuildIndex(library); }
    else if (!cachedRevisions.SameSearchData(library.GetRevisions())) { RefreshMetadata(library); }
    std::vector<std::wstring> queryTokens = Tokenize(normalizedQuery);
    const auto candidates = SelectCandidates(normalizedQuery, queryTokens);
    lastCandidateCount = candidates.second;
    struct ScoredMatch { int score = 0; const CachedSearchEntry* entry = nullptr; };
    std::vector<ScoredMatch> matches;
    const size_t capacity = (std::min)(options.maxResults, candidates.second);
    // Heap selection pays off for small result windows. Large windows keep
    // the full-sort path, avoiding heap-sort overhead for near-total results.
    const bool bounded = capacity < candidates.second / 4;
    matches.reserve(bounded ? capacity : candidates.second);
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
    for (size_t candidateIndex = 0; candidateIndex < candidates.second; ++candidateIndex) {
        const size_t entryIndex = candidates.first ? candidates.first[candidateIndex] : candidateIndex;
        auto& entry = entries[entryIndex];
        if (entry.source == GlobalSearchResultSource::Media && !options.includeMedia) { continue; }
        if (entry.source == GlobalSearchResultSource::History && !options.includeHistory) { continue; }
        if (queryTokens.size() > 1 && !entry.titleTokens.originalText && !entry.pathTokens.originalText) {
            // Token/prefix hits imply a substring in the normalized backing
            // text. Reject impossible multi-word matches before scoring.
            bool possible = true;
            for (const auto& token : queryTokens) {
                if (entry.normalizedTitle.find(token) == std::wstring::npos &&
                    entry.normalizedPath.find(token) == std::wstring::npos) { possible = false; break; }
            }
            if (!possible) continue;
        }
        const int score = CalculateRelevanceScore(entry, normalizedQuery, queryTokens);
        if (score <= 0) continue;
        ++outTotalMatchCount;
        if (capacity == 0) continue;
        RefreshEntryMetadata(entry, entryIndex, library);
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
        item.fileName = src->GetFileNameView();
        item.durationSec = src->durationSec;
        item.lastPosSec = src->lastPosSec;
        item.lastWatchedTimestamp = src->lastWatchedTimestamp;
        item.addedTimestamp = src->addedTimestamp;
        item.source = src->source;
        item.matchScore = matches[i].score;
        if (options.checkFileExists) { item.existsOnDisk = CheckPathExistsOnDisk(src->filePath); } else { item.existsOnDisk = true; }
        results.push_back(std::move(item));
    }
    // Lazy ID/path lookups can build missing lookup maps and advance revision
    // counters. Their content is the same catalog used above; record the final
    // stamps just as the previous eager metadata pass did.
    cachedRevisions = library.GetRevisions();
    cachedSearchContentVersion = library.GetSearchContentVersion();
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
