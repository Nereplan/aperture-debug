#pragma once

#include "../support/TestFramework.h"
#include "../../src/GlobalSearch.h"
#include "../../src/MediaLibrary.h"

#include <vector>
#include <string>
#include <fstream>
#include <cwctype>

namespace Aperture::Test {

// Frozen pre-optimization semantics use owning token strings and exhaustive
// scoring; they do not share compact token ranges or the candidate prefilter.
namespace GlobalSearchReference {
struct Entry {
    std::wstring normalizedTitle, normalizedFileName, normalizedPath;
    std::vector<std::wstring> titleTokens, pathTokens;
};
std::wstring Normalize(const std::wstring& text) {
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

std::vector<std::wstring> Tokenize(const std::wstring& text) {
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

int Score(const Entry& entry, const std::wstring& normalizedQuery,
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

} // namespace GlobalSearchReference

TEST_CASE(GlobalSearch, CompactIndexMatchesReferenceScoresAndOrdering) {
    MediaLibrary library;
    const std::vector<std::wstring> words = {
        L"Alpha Beta", L"alpha.beta", L"Sci-Fi (2024)", L" ALPHA\tGamma ",
        L"日本語 episode", L"İstanbul", L"Straße", L"short", L"\v\f spaced words \r\n",
        std::wstring(L"\0Alpha Beta\0", 12), std::wstring(L"Alpha\0 Beta", 11),
        std::wstring(L"Alpha\0", 6), L"Alpha / Beta", L"", L"..."
    };
    for (size_t i = 0; i < 384; ++i) {
        MediaItem item;
        item.id = std::to_string(i);
        item.title = words[i % words.size()];
        const auto& name = words[(i * 7 + 3) % words.size()];
        item.filePath = LR"(C:\Corpus\)" + std::to_wstring(i) + LR"(\ )" + name + L" .mkv";
        item.addedTimestamp = i % 13; item.lastWatchedTimestamp = i % 17;
        library.mediaItems.push_back(std::move(item));
    }
    GlobalSearchIndex index;
    GlobalSearchQueryOptions options; options.checkFileExists = false;
    std::vector<std::wstring> queries = {L"alpha", L"alpha beta", L"sci fi", L"episode 日本語", L"short", L"absent query", L"mkv", L"corpus alpha", L"spaced words", L".", L"İstanbul", L"straße", std::wstring(L"alpha\0 beta", 11)};
    for (const auto& query : queries) {
        const auto normalized = GlobalSearchReference::Normalize(query);
        const auto tokens = GlobalSearchReference::Tokenize(normalized);
        struct Expected { size_t originalIndex; int score; };
        std::vector<Expected> expected;
        for (size_t i = 0; i < library.mediaItems.size(); ++i) {
            const auto& item = library.mediaItems[i];
            const auto title = item.GetDisplayTitle();
            const auto fileName = GetFileNameFromPath(item.filePath);
            GlobalSearchReference::Entry entry;
            entry.normalizedTitle = GlobalSearchReference::Normalize(title);
            entry.normalizedFileName = GlobalSearchReference::Normalize(fileName);
            entry.normalizedPath = GlobalSearchReference::Normalize(item.filePath);
            entry.titleTokens = GlobalSearchReference::Tokenize(title);
            entry.pathTokens = GlobalSearchReference::Tokenize(item.filePath);
            const int score = GlobalSearchReference::Score(entry, normalized, tokens);
            if (score > 0) expected.push_back({i, score});
        }
        std::sort(expected.begin(), expected.end(), [&](const Expected& a, const Expected& b) {
            if (a.score != b.score) return a.score > b.score;
            const auto& left = library.mediaItems[a.originalIndex]; const auto& right = library.mediaItems[b.originalIndex];
            if (left.lastWatchedTimestamp != right.lastWatchedTimestamp) return left.lastWatchedTimestamp > right.lastWatchedTimestamp;
            if (left.addedTimestamp != right.addedTimestamp) return left.addedTimestamp > right.addedTimestamp;
            const int title = _wcsicmp(left.GetDisplayTitle().c_str(), right.GetDisplayTitle().c_str());
            return title != 0 ? title < 0 : _wcsicmp(left.filePath.c_str(), right.filePath.c_str()) < 0;
        });
        for (const size_t cap : {size_t(0), size_t(1), size_t(50), size_t(1000)}) {
            options.maxResults = cap;
            size_t total = 0;
            const auto actual = index.Query(library, query, options, total);
            TEST_ASSERT_EQ(total, expected.size());
            TEST_ASSERT_EQ(actual.size(), (std::min)(cap, expected.size()));
            for (size_t i = 0; i < actual.size(); ++i) {
                TEST_ASSERT_EQ(actual[i].filePath, library.mediaItems[expected[i].originalIndex].filePath);
                TEST_ASSERT_EQ(actual[i].matchScore, expected[i].score);
                TEST_ASSERT_EQ(actual[i].fileName, GetFileNameFromPath(actual[i].filePath));
            }
        }
    }
    // A larger rebuild moves small-string entries and their relative ranges.
    const std::vector<MediaItem> additional(library.mediaItems.begin(), library.mediaItems.begin() + 40);
    library.mediaItems.insert(library.mediaItems.end(), additional.begin(), additional.end());
    index.Invalidate();
    options.maxResults = 1000;
    const auto results = index.Query(library, L"short", options);
    TEST_ASSERT(!results.empty());
}

TEST_CASE(GlobalSearch, TokenDelimitersMatchReferenceForEveryWideCharacter) {
    std::wstring text;
    text.reserve(65536);
    for (unsigned int c = 0; c <= 65535; ++c) text.push_back(static_cast<wchar_t>(c));
    TEST_ASSERT(GlobalSearchIndex::Tokenize(text) == GlobalSearchReference::Tokenize(text));
}

TEST_CASE(GlobalSearch, HistoryMergeRetainsLastMediaAlias) {
    MediaLibrary library;
    MediaItem first; first.id = "first"; first.filePath = LR"(C:\Aliased\clip.mkv)"; first.title = L"Target first";
    MediaItem second = first; second.id = "second"; second.filePath = L"c:/aliased/clip.mkv"; second.title = L"Target second";
    library.mediaItems = {first, second};
    HistoryRecord history;
    history.filePath = first.filePath; history.title = L"Target history";
    history.watchedTimestamp = 200; history.lastPosSec = 42; history.durationSec = 900;
    library.history.push_back(history);
    GlobalSearchIndex index; GlobalSearchQueryOptions options; options.checkFileExists = false;
    const auto results = index.Query(library, L"target", options);
    TEST_ASSERT_EQ(results.size(), size_t(2));
    // Prefix length contributes to relevance, so verify merge targets by their original paths.
    const auto media = std::find_if(results.begin(), results.end(), [&](const auto& result) { return result.filePath == first.filePath; });
    const auto merged = std::find_if(results.begin(), results.end(), [&](const auto& result) { return result.filePath == second.filePath; });
    TEST_ASSERT(media != results.end()); TEST_ASSERT(merged != results.end());
    if (media == results.end() || merged == results.end()) return;
    TEST_ASSERT_EQ(media->title, first.title);
    TEST_ASSERT(media->source == GlobalSearchResultSource::Media);
    TEST_ASSERT_EQ(merged->title, second.title);
    TEST_ASSERT(merged->source == GlobalSearchResultSource::MediaAndHistory);
    TEST_ASSERT_EQ(merged->lastPosSec, 42.0);
    TEST_ASSERT_EQ(merged->durationSec, 900.0);
}


TEST_CASE(GlobalSearch, PlaybackProgressRefreshesStatisticsWithoutRebuildingText) {
    MediaLibrary library;
    MediaItem item; item.id = "stable_id"; item.filePath = L"C:\\Media\\episode.mkv";
    item.title = L"Episode"; item.durationSec = 100;
    library.mediaItems.push_back(item); library.RebuildPathIndex();
    GlobalSearchIndex index; GlobalSearchQueryOptions options; options.checkFileExists = false;
    TEST_ASSERT_EQ(index.Query(library, L"Episode", options).size(), 1ull);
    const auto contentVersion = library.GetSearchContentVersion();
    library.UpdateProgress(item.filePath, 25, 100);
    const auto updated = index.Query(library, L"Episode", options);
    TEST_ASSERT_EQ(updated.size(), 1ull);
    TEST_ASSERT_EQ(updated.front().lastPosSec, 25.0);
    TEST_ASSERT_EQ(library.GetSearchContentVersion(), contentVersion);
    TEST_ASSERT_EQ(index.GetTextRebuildCount(), 1ull);
    TEST_ASSERT_EQ(index.GetMetadataRefreshCount(), 1ull);
    library.SetWatched(item.filePath, true);
    TEST_ASSERT_EQ(index.Query(library, L"Episode", options).front().lastPosSec, 100.0);
    TEST_ASSERT_EQ(index.GetTextRebuildCount(), 1ull);
    MediaMetadataOverrides overrides; overrides.title = L"Renamed Episode";
    TEST_ASSERT(library.SetMetadataOverrides(item.filePath, overrides));
    TEST_ASSERT_EQ(index.Query(library, L"Renamed", options).front().title, std::wstring(L"Renamed Episode"));
    TEST_ASSERT_EQ(index.GetTextRebuildCount(), 2ull);
}

TEST_CASE(GlobalSearch, PlaylistEditsPreserveWarmTextAndMetadata) {
    MediaLibrary library;
    library.SetSaveRequestHandler([] { return MediaLibrarySaveResult{}; });
    MediaItem item; item.id = "media"; item.title = L"Episode"; item.filePath = L"C:\Media\episode.mkv";
    library.mediaItems.push_back(item); library.RebuildPathIndex();
    GlobalSearchIndex index; GlobalSearchQueryOptions options; options.checkFileExists = false;
    TEST_ASSERT_EQ(index.Query(library, L"Episode", options).size(), 1ull);
    const auto before = library.GetRevisions();
    auto* playlist = library.CreatePlaylist("One");
    TEST_ASSERT(playlist != nullptr);
    playlist->name = "Renamed"; library.NotifyPlaylistEdited();
    TEST_ASSERT(!index.IsDirty(library));
    TEST_ASSERT_EQ(index.Query(library, L"Episode", options).size(), 1ull);
    TEST_ASSERT_EQ(index.GetTextRebuildCount(), 1ull);
    TEST_ASSERT_EQ(index.GetMetadataRefreshCount(), 0ull);
    TEST_ASSERT_EQ(library.GetRevisions().catalog, before.catalog);
    TEST_ASSERT_EQ(library.GetRevisions().history, before.history);
    TEST_ASSERT(library.GetRevisions().playlists > before.playlists);
}

TEST_CASE(GlobalSearch, HistoryMembershipInvalidatesCachedSearch) {
    MediaLibrary library; GlobalSearchIndex index; GlobalSearchQueryOptions options; options.checkFileExists = false;
    TEST_ASSERT(index.Query(library, L"history", options).empty());
    library.AddToHistory(L"C:\\Media\\history-only.mkv", 10, 100);
    auto found = index.Query(library, L"history", options);
    TEST_ASSERT_EQ(found.size(), 1ull);
    TEST_ASSERT_EQ(found.front().source, GlobalSearchResultSource::History);
    TEST_ASSERT(library.RemoveFromHistory(0));
    TEST_ASSERT(index.Query(library, L"history", options).empty());
    library.AddToHistory(L"C:\\Media\\history-only.mkv", 20, 100);
    TEST_ASSERT_EQ(index.Query(library, L"history", options).size(), 1ull);
    library.ClearHistory(); TEST_ASSERT(index.Query(library, L"history", options).empty());
}

TEST_CASE(GlobalSearch, DirectMediaInsertionInvalidatesWarmIndex) {
    MediaLibrary library;
    MediaItem seed; seed.id = "seed"; seed.filePath = L"C:\\Media\\existing.mkv";
    seed.title = L"Existing";
    library.mediaItems.push_back(seed); library.RebuildPathIndex();
    library.SetSaveRequestHandler([] { return MediaLibrarySaveResult{}; });
    GlobalSearchIndex index; GlobalSearchQueryOptions options; options.checkFileExists = false;
    TEST_ASSERT_EQ(index.Query(library, L"Existing", options).size(), 1ull);
    TEST_ASSERT(index.Query(library, L"NewlyAdded", options).empty());
    const auto contentVersion = library.GetSearchContentVersion();
    const auto* added = library.AddOrUpdateMedia(L"C:\\Media\\NewlyAdded.mkv", false);
    TEST_ASSERT(added != nullptr);
    TEST_ASSERT(library.GetSearchContentVersion() > contentVersion);
    const auto found = index.Query(library, L"NewlyAdded", options);
    TEST_ASSERT_EQ(found.size(), 1ull);
    TEST_ASSERT_EQ(found.front().filePath, std::wstring(L"C:\\Media\\NewlyAdded.mkv"));
    TEST_ASSERT_EQ(index.GetTextRebuildCount(), 2ull);
    TEST_ASSERT(library.history.empty());
}

// ============================================================================
// 1. Normalization & Tokenization Tests
// ============================================================================

TEST_CASE(GlobalSearch, NormalizationBasics) {
    // Leading and trailing whitespace trimming
    TEST_ASSERT_EQ(GlobalSearchIndex::NormalizeText(L"   hello world   "), std::wstring(L"hello world"));

    // Case folding
    TEST_ASSERT_EQ(GlobalSearchIndex::NormalizeText(L"Big BUCK Bunny"), std::wstring(L"big buck bunny"));

    // Path separator normalization: forward slash converted to backslash
    TEST_ASSERT_EQ(GlobalSearchIndex::NormalizeText(L"movies/trailers/sample.mp4"), std::wstring(L"movies\\trailers\\sample.mp4"));

    // Multiple consecutive whitespaces collapsed to single space
    TEST_ASSERT_EQ(GlobalSearchIndex::NormalizeText(L"Action    Movie   2024   4K"), std::wstring(L"action movie 2024 4k"));

    // Whitespace only and empty string
    TEST_ASSERT_EQ(GlobalSearchIndex::NormalizeText(L""), std::wstring(L""));
    TEST_ASSERT_EQ(GlobalSearchIndex::NormalizeText(L"    \t\r\n   "), std::wstring(L""));

    // Mixed path, tabs, and case
    TEST_ASSERT_EQ(GlobalSearchIndex::NormalizeText(L" \t C:/Videos/Anime/Episode 01.MKV \n "),
                   std::wstring(L"c:\\videos\\anime\\episode 01.mkv"));
}

TEST_CASE(GlobalSearch, Tokenization) {
    auto tokens = GlobalSearchIndex::Tokenize(L"Big_Buck.Bunny-2024 [1080p] (HEVC).mkv");
    std::vector<std::wstring> expected = {
        L"big", L"buck", L"bunny", L"2024", L"1080p", L"hevc", L"mkv"
    };

    TEST_ASSERT_EQ(tokens.size(), expected.size());
    for (size_t i = 0; i < (std::min)(tokens.size(), expected.size()); ++i) {
        TEST_ASSERT_EQ(tokens[i], expected[i]);
    }

    // Path tokenization
    auto pathTokens = GlobalSearchIndex::Tokenize(L"D:\\Media\\Sci-Fi\\Interstellar (2014)\\movie.mp4");
    TEST_ASSERT(pathTokens.size() >= 5);
    TEST_ASSERT_EQ(pathTokens[0], std::wstring(L"d"));
    TEST_ASSERT_EQ(pathTokens[1], std::wstring(L"media"));
    TEST_ASSERT_EQ(pathTokens[2], std::wstring(L"sci"));
    TEST_ASSERT_EQ(pathTokens[3], std::wstring(L"fi"));
    TEST_ASSERT_EQ(pathTokens[4], std::wstring(L"interstellar"));

    // Empty text
    auto emptyTokens = GlobalSearchIndex::Tokenize(L"");
    TEST_ASSERT(emptyTokens.empty());
}

TEST_CASE(GlobalSearch, ExtractFileName) {
    TEST_ASSERT_EQ(GlobalSearchIndex::ExtractFileName(L"C:\\Videos\\Movie.mp4"), std::wstring(L"Movie.mp4"));
    TEST_ASSERT_EQ(GlobalSearchIndex::ExtractFileName(L"D:/Downloads/Clip.mkv"), std::wstring(L"Clip.mkv"));
    TEST_ASSERT_EQ(GlobalSearchIndex::ExtractFileName(L"Standalone.avi"), std::wstring(L"Standalone.avi"));
    TEST_ASSERT_EQ(GlobalSearchIndex::ExtractFileName(L""), std::wstring(L""));
}

// ============================================================================
// 2. Relevance & Recency Deterministic Tie-Breaking
// ============================================================================

TEST_CASE(GlobalSearch, RelevanceOrdering) {
    MediaLibrary lib;
    GlobalSearchIndex index;

    // Item 1: Exact title match
    MediaItem item1;
    item1.filePath = L"C:\\Media\\Matrix.mp4";
    item1.title = L"Matrix";
    item1.addedTimestamp = 100;
    lib.mediaItems.push_back(item1);

    // Item 2: Prefix title match
    MediaItem item2;
    item2.filePath = L"C:\\Media\\Matrix Reloaded.mp4";
    item2.title = L"Matrix Reloaded";
    item2.addedTimestamp = 200;
    lib.mediaItems.push_back(item2);

    // Item 3: Substring title match
    MediaItem item3;
    item3.filePath = L"C:\\Media\\The Matrix Revolutions.mp4";
    item3.title = L"The Matrix Revolutions";
    item3.addedTimestamp = 300;
    lib.mediaItems.push_back(item3);

    // Item 4: Path only match
    MediaItem item4;
    item4.filePath = L"C:\\Media\\Matrix_Collection\\Animatrix.mp4";
    item4.title = L"Animatrix";
    item4.addedTimestamp = 400;
    lib.mediaItems.push_back(item4);

    GlobalSearchQueryOptions opts;
    opts.checkFileExists = false;

    size_t total = 0;
    auto results = index.Query(lib, L"Matrix", opts, total);

    TEST_ASSERT_EQ(results.size(), 4);
    TEST_ASSERT_EQ(total, 4);

    // Exact title match must be #1
    TEST_ASSERT_EQ(results[0].title, std::wstring(L"Matrix"));
    // Prefix title match must be #2
    TEST_ASSERT_EQ(results[1].title, std::wstring(L"Matrix Reloaded"));
    // Substring title match must be #3
    TEST_ASSERT_EQ(results[2].title, std::wstring(L"The Matrix Revolutions"));
    // Path-only match must be #4
    TEST_ASSERT_EQ(results[3].title, std::wstring(L"Animatrix"));
}

TEST_CASE(GlobalSearch, RecencyAndDeterministicTieBreaking) {
    MediaLibrary lib;
    GlobalSearchIndex index;

    // Two items with identical title score, but different lastWatchedTimestamp
    MediaItem itemA;
    itemA.filePath = L"C:\\Media\\Alpha 1.mp4";
    itemA.title = L"Alpha Episode";
    itemA.lastWatchedTimestamp = 1000;
    itemA.addedTimestamp = 10;
    lib.mediaItems.push_back(itemA);

    MediaItem itemB;
    itemB.filePath = L"C:\\Media\\Alpha 2.mp4";
    itemB.title = L"Alpha Episode";
    itemB.lastWatchedTimestamp = 2000; // More recently watched
    itemB.addedTimestamp = 20;
    lib.mediaItems.push_back(itemB);

    // Item C: Same score, same watched timestamp, different added timestamp
    MediaItem itemC;
    itemC.filePath = L"C:\\Media\\Alpha 3.mp4";
    itemC.title = L"Alpha Episode";
    itemC.lastWatchedTimestamp = 1000;
    itemC.addedTimestamp = 50; // Newer added
    lib.mediaItems.push_back(itemC);

    // Item D: Same score, same watched, same added, different path
    MediaItem itemD;
    itemD.filePath = L"C:\\Media\\Alpha 0.mp4"; // Lexicographically earlier path
    itemD.title = L"Alpha Episode";
    itemD.lastWatchedTimestamp = 1000;
    itemD.addedTimestamp = 10;
    lib.mediaItems.push_back(itemD);

    GlobalSearchQueryOptions opts;
    opts.checkFileExists = false;

    size_t total = 0;
    auto results = index.Query(lib, L"Alpha Episode", opts, total);

    TEST_ASSERT_EQ(results.size(), 4);

    // 1st: itemB (lastWatched = 2000)
    TEST_ASSERT_EQ(results[0].filePath, std::wstring(L"C:\\Media\\Alpha 2.mp4"));

    // 2nd: itemC (lastWatched = 1000, added = 50)
    TEST_ASSERT_EQ(results[1].filePath, std::wstring(L"C:\\Media\\Alpha 3.mp4"));

    // 3rd & 4th: same watched (1000) and added (10), tie broken by path ascending
    // "Alpha 0.mp4" < "Alpha 1.mp4"
    TEST_ASSERT_EQ(results[2].filePath, std::wstring(L"C:\\Media\\Alpha 0.mp4"));
    TEST_ASSERT_EQ(results[3].filePath, std::wstring(L"C:\\Media\\Alpha 1.mp4"));
}

TEST_CASE(GlobalSearch, MultiTokenSearch) {
    MediaLibrary lib;
    GlobalSearchIndex index;

    MediaItem item1;
    item1.filePath = L"C:\\Videos\\Bunny_Trailer.mp4";
    item1.title = L"Big Buck Bunny Official 4K Trailer";
    lib.mediaItems.push_back(item1);

    MediaItem item2;
    item2.filePath = L"C:\\Videos\\Sintel_Trailer.mp4";
    item2.title = L"Sintel Animation Trailer";
    lib.mediaItems.push_back(item2);

    GlobalSearchQueryOptions opts;
    opts.checkFileExists = false;

    // Search for "bunny trailer" -> should match item1 but NOT item2
    size_t total = 0;
    auto results = index.Query(lib, L"bunny trailer", opts, total);

    TEST_ASSERT_EQ(results.size(), 1);
    TEST_ASSERT_EQ(total, 1);
    TEST_ASSERT_EQ(results[0].title, std::wstring(L"Big Buck Bunny Official 4K Trailer"));
}

// ============================================================================
// 3. Result Bounding & Caps
// ============================================================================

TEST_CASE(GlobalSearch, ResultCapsAndBounding) {
    MediaLibrary lib;
    GlobalSearchIndex index;

    // Add 100 sample items
    for (int i = 0; i < 100; ++i) {
        MediaItem item;
        item.filePath = L"C:\\Media\\Video_" + std::to_wstring(i) + L".mp4";
        item.title = L"Sample Video " + std::to_wstring(i);
        item.addedTimestamp = i;
        lib.mediaItems.push_back(item);
    }

    GlobalSearchQueryOptions opts;
    opts.checkFileExists = false;
    opts.maxResults = 10;

    size_t total = 0;
    auto results = index.Query(lib, L"Sample Video", opts, total);

    // Results must be capped to maxResults
    TEST_ASSERT_EQ(results.size(), 10);
    // Total match count must report all 100 matches
    TEST_ASSERT_EQ(total, 100);

    // Custom cap: 25
    opts.maxResults = 25;
    results = index.Query(lib, L"Sample Video", opts, total);
    TEST_ASSERT_EQ(results.size(), 25);
    TEST_ASSERT_EQ(total, 100);

    // Cap larger than total: 200
    opts.maxResults = 200;
    results = index.Query(lib, L"Sample Video", opts, total);
    TEST_ASSERT_EQ(results.size(), 100);
    TEST_ASSERT_EQ(total, 100);
}

// ============================================================================
// 4. Cache Invalidation & Version Tracking
// ============================================================================

TEST_CASE(GlobalSearch, CacheInvalidationAndRebuild) {
    MediaLibrary lib;
    GlobalSearchIndex index;

    MediaItem item1;
    item1.filePath = L"C:\\Media\\Item1.mp4";
    item1.title = L"First Movie";
    lib.mediaItems.push_back(item1);

    GlobalSearchQueryOptions opts;
    opts.checkFileExists = false;

    // First query builds the index
    auto results = index.Query(lib, L"First", opts);
    TEST_ASSERT_EQ(results.size(), 1);
    TEST_ASSERT_EQ(index.GetEntryCount(), 1);
    TEST_ASSERT(!index.IsDirty(lib));

    // Add a second media item to library (increases libraryVersion)
    lib.AddOrUpdateMedia(L"C:\\Media\\Item2.mp4");
    MediaItem* item2 = lib.FindByPath(L"C:\\Media\\Item2.mp4");
    TEST_ASSERT(item2 != nullptr);
    if (item2) item2->title = L"Second Movie";

    // Index is now dirty
    TEST_ASSERT(index.IsDirty(lib));

    // Next query automatically invalidates cache and rebuilds
    results = index.Query(lib, L"Second", opts);
    TEST_ASSERT_EQ(results.size(), 1);
    TEST_ASSERT_EQ(results[0].title, std::wstring(L"Second Movie"));
    TEST_ASSERT(!index.IsDirty(lib));
    TEST_ASSERT_EQ(index.GetEntryCount(), 2);

    // Manual invalidation
    index.Invalidate();
    TEST_ASSERT(index.IsDirty(lib));
    TEST_ASSERT_EQ(index.GetEntryCount(), 0);
}

TEST_CASE(GlobalSearch, DeduplicationAndSourceMerging) {
    MediaLibrary lib;
    GlobalSearchIndex index;

    // Item in media library
    MediaItem item;
    item.filePath = L"C:\\Media\\Shared.mp4";
    item.title = L"Shared Title";
    item.durationSec = 120.0;
    item.lastPosSec = 10.0;
    item.lastWatchedTimestamp = 100;
    lib.mediaItems.push_back(item);

    // Same item path in history with newer watch timestamp and position
    HistoryRecord hist;
    hist.filePath = L"C:\\Media\\Shared.mp4";
    hist.title = L"Shared Title";
    hist.durationSec = 120.0;
    hist.lastPosSec = 60.0;
    hist.watchedTimestamp = 500;
    lib.history.push_back(hist);

    GlobalSearchQueryOptions opts;
    opts.checkFileExists = false;

    auto results = index.Query(lib, L"Shared", opts);

    // Must be de-duplicated into 1 entry
    TEST_ASSERT_EQ(results.size(), 1);
    TEST_ASSERT_EQ(results[0].source, GlobalSearchResultSource::MediaAndHistory);
    // Preserves the latest history timestamp and playback position
    TEST_ASSERT_EQ(results[0].lastWatchedTimestamp, 500);
    TEST_ASSERT_EQ(results[0].lastPosSec, 60.0);
    TEST_ASSERT_EQ(results[0].GetSourceDisplayName(), std::wstring(L"Library & History"));
}

// ============================================================================
// 5. Missing History Targets & Action Resolution
// ============================================================================

TEST_CASE(GlobalSearch, MissingHistoryTargetHandling) {
    MediaLibrary lib;
    GlobalSearchModel model(0); // 0ms debounce for unit test

    // History-only entry pointing to a non-existent file
    HistoryRecord hist;
    hist.filePath = L"Z:\\NonExistentDrive\\GhostMovie_12345.mp4";
    hist.title = L"Ghost Movie";
    hist.lastPosSec = 45.0;
    hist.watchedTimestamp = 1000;
    lib.history.push_back(hist);

    model.SetQuery(L"Ghost", lib, true);
    auto proj = model.GetProjection();

    TEST_ASSERT_EQ(proj.results.size(), 1);
    TEST_ASSERT_EQ(proj.results[0].source, GlobalSearchResultSource::History);
    // Non-existent file marked existsOnDisk = false
    TEST_ASSERT(!proj.results[0].existsOnDisk);

    // Activate selection on missing media returns ReportMissingMedia safely
    model.SetSelectedIndex(0);
    auto action = model.ActivateSelection(lib);

    TEST_ASSERT_EQ(action.action, GlobalSearchActionType::ReportMissingMedia);
    TEST_ASSERT(action.isMissing);
    TEST_ASSERT(action.message.find(L"GhostMovie_12345.mp4") != std::wstring::npos);

    // Explorer action on missing media also reports missing safely
    auto explorerAction = model.OpenExplorerForSelection(lib);
    TEST_ASSERT_EQ(explorerAction.action, GlobalSearchActionType::ReportMissingMedia);
    TEST_ASSERT(explorerAction.isMissing);
}

// ============================================================================
// 6. Keyboard Selection & Navigation
// ============================================================================

TEST_CASE(GlobalSearch, KeyboardNavigationAndClamping) {
    MediaLibrary lib;
    GlobalSearchModel model(0);

    for (int i = 0; i < 10; ++i) {
        MediaItem item;
        item.filePath = L"C:\\Media\\Ep_" + std::to_wstring(i) + L".mp4";
        item.title = L"Episode " + std::to_wstring(i);
        lib.mediaItems.push_back(item);
    }

    GlobalSearchQueryOptions opts;
    opts.checkFileExists = false;
    model.SetQueryOptions(opts);

    model.SetQuery(L"Episode", lib, true);
    auto proj = model.GetProjection();
    TEST_ASSERT_EQ(proj.results.size(), 10);

    // Initial selected index is 0
    TEST_ASSERT_EQ(model.GetSelectedIndex(), 0);

    // Move down
    model.MoveSelectionDown();
    TEST_ASSERT_EQ(model.GetSelectedIndex(), 1);

    model.MoveSelectionDown();
    TEST_ASSERT_EQ(model.GetSelectedIndex(), 2);

    // Move up
    model.MoveSelectionUp();
    TEST_ASSERT_EQ(model.GetSelectedIndex(), 1);

    // Move home & end
    model.MoveSelectionEnd();
    TEST_ASSERT_EQ(model.GetSelectedIndex(), 9);

    // Move down at end without wrap -> clamps at 9
    model.MoveSelectionDown(false);
    TEST_ASSERT_EQ(model.GetSelectedIndex(), 9);

    // Move down at end with wrap -> wraps to 0
    model.MoveSelectionDown(true);
    TEST_ASSERT_EQ(model.GetSelectedIndex(), 0);

    // Move up at start with wrap -> wraps to 9
    model.MoveSelectionUp(true);
    TEST_ASSERT_EQ(model.GetSelectedIndex(), 9);

    model.MoveSelectionHome();
    TEST_ASSERT_EQ(model.GetSelectedIndex(), 0);

    // Page down & Page up
    model.MoveSelectionPageDown(4);
    TEST_ASSERT_EQ(model.GetSelectedIndex(), 4);

    model.MoveSelectionPageUp(3);
    TEST_ASSERT_EQ(model.GetSelectedIndex(), 1);

    // Selection clamping when query narrows results
    model.SetSelectedIndex(8);
    TEST_ASSERT_EQ(model.GetSelectedIndex(), 8);

    // Search for specific episode -> only 1 result
    model.SetQuery(L"Episode 3", lib, true);
    TEST_ASSERT_EQ(model.GetProjection().results.size(), 1);
    // Index automatically clamped to 0
    TEST_ASSERT_EQ(model.GetSelectedIndex(), 0);

    // Clear selection
    model.ClearSelection();
    TEST_ASSERT_EQ(model.GetSelectedIndex(), -1);
}

// ============================================================================
// 7. Empty Query & Whitespace Handling
// ============================================================================

TEST_CASE(GlobalSearch, EmptyQueryBehavior) {
    MediaLibrary lib;
    GlobalSearchModel model(0);

    MediaItem item;
    item.filePath = L"C:\\Media\\Test.mp4";
    item.title = L"Test Video";
    lib.mediaItems.push_back(item);

    // Empty query
    model.SetQuery(L"", lib, true);
    auto proj = model.GetProjection();
    TEST_ASSERT(proj.results.empty());
    TEST_ASSERT_EQ(proj.totalMatchCount, 0);
    TEST_ASSERT_EQ(proj.selectedIndex, -1);
    TEST_ASSERT(!proj.HasSelection());
    TEST_ASSERT(proj.GetSelectedItem() == nullptr);

    // Whitespace only query
    model.SetQuery(L"    \t\n   ", lib, true);
    proj = model.GetProjection();
    TEST_ASSERT(proj.results.empty());
    TEST_ASSERT_EQ(proj.totalMatchCount, 0);
    TEST_ASSERT_EQ(proj.selectedIndex, -1);
}

// ============================================================================
// 8. Immutable Projection & State Isolation
// ============================================================================

TEST_CASE(GlobalSearch, ImmutableProjectionSnapshot) {
    MediaLibrary lib;
    GlobalSearchModel model(0);

    MediaItem item1;
    item1.filePath = L"C:\\Media\\A.mp4";
    item1.title = L"Alpha One";
    lib.mediaItems.push_back(item1);

    MediaItem item2;
    item2.filePath = L"C:\\Media\\B.mp4";
    item2.title = L"Beta Two";
    lib.mediaItems.push_back(item2);

    GlobalSearchQueryOptions opts;
    opts.checkFileExists = false;
    model.SetQueryOptions(opts);

    model.SetQuery(L"Alpha", lib, true);
    GlobalSearchProjection snap1 = model.GetProjection();

    TEST_ASSERT_EQ(snap1.query, std::wstring(L"Alpha"));
    TEST_ASSERT_EQ(snap1.results.size(), 1);
    TEST_ASSERT_EQ(snap1.results[0].title, std::wstring(L"Alpha One"));
    TEST_ASSERT_EQ(snap1.selectedIndex, 0);

    // Now change model query and selection
    model.SetQuery(L"Beta", lib, true);
    model.MoveSelectionDown();
    GlobalSearchProjection snap2 = model.GetProjection();

    // snap1 remains completely untouched and immutable
    TEST_ASSERT_EQ(snap1.query, std::wstring(L"Alpha"));
    TEST_ASSERT_EQ(snap1.results.size(), 1);
    TEST_ASSERT_EQ(snap1.results[0].title, std::wstring(L"Alpha One"));

    // snap2 reflects the new state
    TEST_ASSERT_EQ(snap2.query, std::wstring(L"Beta"));
    TEST_ASSERT_EQ(snap2.results.size(), 1);
    TEST_ASSERT_EQ(snap2.results[0].title, std::wstring(L"Beta Two"));
}

// ============================================================================
// 9. Debounce Behavior
// ============================================================================

TEST_CASE(GlobalSearch, DebounceTiming) {
    MediaLibrary lib;
    GlobalSearchModel model(150); // 150ms debounce

    MediaItem item;
    item.filePath = L"C:\\Media\\Sample.mp4";
    item.title = L"Sample Video";
    lib.mediaItems.push_back(item);

    GlobalSearchQueryOptions opts;
    opts.checkFileExists = false;
    model.SetQueryOptions(opts);

    // Non-immediate SetQuery initiates debounce
    model.SetQuery(L"Sample", lib, false);
    auto proj = model.GetProjection();

    TEST_ASSERT(proj.isDebouncing);
    // Results not yet calculated
    TEST_ASSERT(proj.results.empty());

    // Tick 50ms (not expired)
    bool updated = model.Tick(50, lib);
    TEST_ASSERT(!updated);
    TEST_ASSERT(model.GetProjection().isDebouncing);

    // Tick another 100ms (50 + 100 = 150ms -> expired)
    updated = model.Tick(100, lib);
    TEST_ASSERT(updated);

    proj = model.GetProjection();
    TEST_ASSERT(!proj.isDebouncing);
    TEST_ASSERT_EQ(proj.results.size(), 1);
    TEST_ASSERT_EQ(proj.results[0].title, std::wstring(L"Sample Video"));
}

// ============================================================================
// 10. Source Filtering & Formatted Helpers
// ============================================================================

TEST_CASE(GlobalSearch, SourceFilteringAndFormatting) {
    MediaLibrary lib;
    GlobalSearchIndex index;

    MediaItem item;
    item.filePath = L"C:\\Media\\Movie.mp4";
    item.title = L"Movie in Library";
    item.durationSec = 3665.0; // 1:01:05
    lib.mediaItems.push_back(item);

    HistoryRecord hist;
    hist.filePath = L"C:\\History\\Clip.mp4";
    hist.title = L"Clip in History";
    hist.durationSec = 125.0; // 2:05
    lib.history.push_back(hist);

    GlobalSearchQueryOptions opts;
    opts.checkFileExists = false;

    // Filter: Include Media only
    opts.includeMedia = true;
    opts.includeHistory = false;
    size_t total = 0;
    auto results = index.Query(lib, L"in", opts, total);
    TEST_ASSERT_EQ(results.size(), 1);
    TEST_ASSERT_EQ(results[0].title, std::wstring(L"Movie in Library"));
    TEST_ASSERT_EQ(results[0].GetFormattedDuration(), std::wstring(L"1:01:05"));
    TEST_ASSERT_EQ(results[0].GetSourceDisplayName(), std::wstring(L"Library"));

    // Filter: Include History only
    opts.includeMedia = false;
    opts.includeHistory = true;
    results = index.Query(lib, L"in", opts, total);
    TEST_ASSERT_EQ(results.size(), 1);
    TEST_ASSERT_EQ(results[0].title, std::wstring(L"Clip in History"));
    TEST_ASSERT_EQ(results[0].GetFormattedDuration(), std::wstring(L"2:05"));
    TEST_ASSERT_EQ(results[0].GetSourceDisplayName(), std::wstring(L"History"));
}

TEST_CASE(GlobalSearch, BoundedRankingMatchesFullOrdering) {
    MediaLibrary lib;
    // Reverse input order and vary relevance and both recency tie-breakers.
    for (int i = 399; i >= 0; --i) {
        MediaItem item;
        item.filePath = L"C:\\Search\\clip_" + std::to_wstring(i) + L".mkv";
        item.title = i % 3 == 0 ? L"Clip" : (i % 3 == 1 ? L"Clip extended" : L"A Clip");
        item.lastWatchedTimestamp = i % 7;
        item.addedTimestamp = i % 11;
        lib.mediaItems.push_back(std::move(item));
    }
    GlobalSearchIndex index;
    GlobalSearchQueryOptions options;
    options.checkFileExists = false;
    options.maxResults = 1000;
    size_t total = 0;
    const auto full = index.Query(lib, L"clip", options, total);
    TEST_ASSERT_EQ(total, size_t(400));
    for (size_t cap : {size_t(0), size_t(1), size_t(2), size_t(50), size_t(399), size_t(400), size_t(1000)}) {
        options.maxResults = cap;
        const auto limited = index.Query(lib, L"clip", options, total);
        TEST_ASSERT_EQ(total, full.size());
        TEST_ASSERT_EQ(limited.size(), (std::min)(cap, full.size()));
        for (size_t i = 0; i < limited.size(); ++i) {
            TEST_ASSERT_EQ(limited[i].filePath, full[i].filePath);
            TEST_ASSERT_EQ(limited[i].matchScore, full[i].matchScore);
        }
    }
    options.maxResults = 50;
    TEST_ASSERT(index.Query(lib, L"no such match", options, total).empty());
    TEST_ASSERT_EQ(total, size_t(0));
}

TEST_CASE(GlobalSearch, EmptyQueryDefersIndexBuild) {
    MediaLibrary lib;
    MediaItem item;
    item.filePath = L"C:\\Search\\Clip.mkv";
    item.title = L"Clip";
    lib.mediaItems.push_back(item);
    GlobalSearchIndex index;
    GlobalSearchQueryOptions options;
    options.checkFileExists = false;
    size_t total = 123;
    TEST_ASSERT(index.Query(lib, L" \t\r\n ", options, total).empty());
    TEST_ASSERT_EQ(total, size_t(0));
    TEST_ASSERT_EQ(index.GetEntryCount(), size_t(0));
    TEST_ASSERT(index.IsDirty(lib));
    TEST_ASSERT_EQ(index.Query(lib, L"clip", options, total).size(), size_t(1));
    TEST_ASSERT(!index.IsDirty(lib));
}

} // namespace Aperture::Test
