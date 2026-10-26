#include "Podcasts.h"
#include "Common.h"
#include "XmlLite.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <ctime>

namespace Aperture {

namespace {

using XmlLite::DecodeXmlEntities;
using XmlLite::ExtractXmlAttribute;
using XmlLite::ExtractXmlElement;
using XmlLite::ToLowerAsciiCopy;
using XmlLite::Trim;

// The cache file wraps the raw feed XML with a wall-clock freshness stamp:
// { "cachedAtSec": <unix time>, "body": "<raw feed XML>" }. Same pattern as the
// internet-radio stations cache.
bool LoadPodcastCacheImpl(const std::wstring& path, std::string& outBody, bool& outFresh) {
    outBody.clear();
    outFresh = false;
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(hFile, &size) && size.QuadPart > 0 && size.QuadPart <= 36 * 1024 * 1024;
    std::string content;
    if (ok) {
        content.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(hFile, content.data(), static_cast<DWORD>(content.size()), &read, NULL) && read == content.size();
    }
    CloseHandle(hFile);
    if (!ok) return false;
    JsonValue root;
    if (!JsonValue::parse(content, root) || !root.is_object()) return false;
    const int64_t cachedAt = root.get("cachedAtSec").as_int64(0);
    if (cachedAt <= 0) return false;
    const std::string body = root.get("body").as_string("");
    if (body.empty()) return false;
    outBody = body;
    outFresh = (time(nullptr) - cachedAt) < PodcastRefreshService::kCacheFreshnessSec;
    return true;
}

StorageStatus SavePodcastCacheImpl(const std::wstring& path, const std::string& body, int64_t nowSec) {
    JsonValue wrapper = JsonValue::Object();
    wrapper["cachedAtSec"] = static_cast<int64_t>(nowSec);
    wrapper["body"] = body;
    const std::string content = wrapper.stringify();
    // Atomic write sequence: temp file next to the target, flush, then rename.
    std::wstring tmpPath = path + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetCurrentThreadId()) + L"." + std::to_wstring(GetTickCount64());
    HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return StorageStatus::Error(StorageErrorCode::FileOpenFailed, err, tmpPath, "Failed to create temporary podcast cache file");
    }
    DWORD written = 0;
    BOOL writeOk = WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()), &written, NULL);
    writeOk = writeOk && (written == static_cast<DWORD>(content.size()));
    DWORD writeErr = writeOk ? 0 : GetLastError();
    if (writeOk) { FlushFileBuffers(hFile); }
    CloseHandle(hFile);
    if (!writeOk) {
        DeleteFileW(tmpPath.c_str());
        return StorageStatus::Error(StorageErrorCode::FileWriteFailed, writeErr, tmpPath, "Failed to write podcast cache data");
    }
    if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD moveErr = GetLastError();
        DeleteFileW(tmpPath.c_str());
        return StorageStatus::Error(StorageErrorCode::AtomicRenameFailed, moveErr, path, "Failed to atomically rename podcast cache file");
    }
    return StorageStatus::Ok(path);
}

// RFC822 <pubDate>: optional "Day, " prefix, then "dd Mon yyyy hh:mm:ss"
// followed by a zone offset (+HHMM/-HHMM) or a named zone (GMT/UT/UTC and the
// common US zone names). Anything unparseable yields 0.
int64_t ParseRfc822Date(const std::string& text) {
    std::string value = Trim(text);
    if (value.empty()) return 0;
    // Drop the optional weekday prefix ("Mon, ").
    const size_t comma = value.find(',');
    if (comma != std::string::npos && comma <= 10) value = Trim(value.substr(comma + 1));
    const auto readInt = [&value](size_t& pos) {
        size_t begin = pos;
        while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos]))) ++pos;
        if (begin == pos) return -1;
        try { return std::stoi(value.substr(begin, pos - begin)); } catch (...) { return -1; }
    };
    // Day: leading run of digits.
    size_t cursor = 0;
    while (cursor < value.size() && !std::isdigit(static_cast<unsigned char>(value[cursor]))) ++cursor;
    const int day = readInt(cursor);
    if (day < 0) return 0;
    // Month name: next run of letters.
    while (cursor < value.size() && !std::isalpha(static_cast<unsigned char>(value[cursor]))) ++cursor;
    size_t monthBegin = cursor;
    while (cursor < value.size() && std::isalpha(static_cast<unsigned char>(value[cursor]))) ++cursor;
    static const char* const kMonths[12] = {
        "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"
    };
    const std::string monthToken = ToLowerAsciiCopy(value.substr(monthBegin, cursor - monthBegin));
    int month = 0;
    for (int i = 0; i < 12; ++i) {
        if (monthToken.rfind(kMonths[i], 0) == 0) { month = i + 1; break; } // "september" matches "sep"
    }
    if (month == 0) return 0;
    // Year: next run of digits.
    while (cursor < value.size() && !std::isdigit(static_cast<unsigned char>(value[cursor]))) ++cursor;
    const int year = readInt(cursor);
    if (year < 0) return 0;
    // Time: hh:mm[:ss]; the seconds part is optional.
    while (cursor < value.size() && !std::isdigit(static_cast<unsigned char>(value[cursor]))) ++cursor;
    const int hour = readInt(cursor);
    if (hour < 0 || cursor >= value.size() || value[cursor] != ':') return 0;
    ++cursor;
    const int minute = readInt(cursor);
    if (minute < 0) return 0;
    int second = 0;
    if (cursor < value.size() && value[cursor] == ':') {
        ++cursor;
        second = readInt(cursor);
        if (second < 0) return 0;
    }
    if (year < 1970 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) return 0;
    // Zone: signed offset (+HHMM/-HHMM) or a named zone; unknown names read as UTC.
    int64_t offsetSec = 0;
    while (cursor < value.size() && std::isspace(static_cast<unsigned char>(value[cursor]))) ++cursor;
    if (cursor < value.size() && (value[cursor] == '+' || value[cursor] == '-')) {
        const int sign = value[cursor] == '-' ? -1 : 1;
        ++cursor;
        const int zoneDigits = readInt(cursor);
        if (zoneDigits >= 0) {
            const int zoneHours = zoneDigits / 100, zoneMinutes = zoneDigits % 100;
            if (zoneMinutes > 59) return 0;
            offsetSec = sign * (zoneHours * 3600 + zoneMinutes * 60);
        }
    }
    std::tm tmValue{};
    tmValue.tm_year = year - 1900;
    tmValue.tm_mon = month - 1;
    tmValue.tm_mday = day;
    tmValue.tm_hour = hour;
    tmValue.tm_min = minute;
    tmValue.tm_sec = second;
    tmValue.tm_isdst = 0;
    const time_t utc = _mkgmtime(&tmValue);
    if (utc < 0) return 0;
    return static_cast<int64_t>(utc) - offsetSec;
}

// <itunes:duration>: "H:MM:SS", "MM:SS", or plain seconds; 0 when absent/invalid.
double ParseItunesDuration(const std::string& text) {
    const std::string value = Trim(text);
    if (value.empty()) return 0.0;
    std::vector<int64_t> parts;
    size_t begin = 0;
    for (size_t i = 0; i <= value.size(); ++i) {
        if (i == value.size() || value[i] == ':') {
            const std::string part = value.substr(begin, i - begin);
            if (part.empty() || part.size() > 5 ||
                !std::all_of(part.begin(), part.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
                return 0.0;
            }
            try { parts.push_back(std::stoll(part)); } catch (...) { return 0.0; }
            begin = i + 1;
        }
    }
    if (parts.size() > 3) return 0.0;
    int64_t totalSec = 0;
    for (int64_t units : parts) totalSec = totalSec * 60 + units;
    if (totalSec < 0) return 0.0;
    return static_cast<double>(totalSec);
}

// The enclosure tag is self-closing or paired; grab the full tag text first,
// then read its url attribute (falling back to <media:content url="..."> or
// <link rel="enclosure" href="..."> when feeds use Media RSS / Atom links).
std::string ExtractEnclosureUrl(const std::string& itemBlock) {
    const std::string lower = ToLowerAsciiCopy(itemBlock);
    auto findTagAttr = [&](const char* tagPrefix, const char* attrName, const char* reqAttr = nullptr, const char* reqVal = nullptr) -> std::string {
        size_t searchFrom = 0;
        const size_t prefixLen = std::strlen(tagPrefix);
        while (searchFrom < lower.size()) {
            size_t open = lower.find(tagPrefix, searchFrom);
            if (open == std::string::npos) return "";
            size_t after = open + prefixLen;
            if (after < lower.size() && lower[after] != '>' && lower[after] != ' ' && lower[after] != '\t' &&
                lower[after] != '\r' && lower[after] != '\n' && lower[after] != '/') {
                searchFrom = open + 1;
                continue;
            }
            size_t close = lower.find('>', after);
            if (close == std::string::npos) return "";
            const std::string tag = itemBlock.substr(open, close - open + 1);
            if (reqAttr && reqVal) {
                const std::string rv = ToLowerAsciiCopy(Trim(ExtractXmlAttribute(tag, reqAttr)));
                if (rv != reqVal) { searchFrom = close + 1; continue; }
            }
            std::string url = Trim(ExtractXmlAttribute(tag, attrName));
            if (url.size() >= 12 && url.rfind("<![CDATA[", 0) == 0 && url.compare(url.size() - 3, 3, "]]>") == 0) {
                url = Trim(url.substr(9, url.size() - 12));
            }
            if (!url.empty()) return url;
            searchFrom = close + 1;
        }
        return "";
    };
    std::string url = findTagAttr("<enclosure", "url");
    if (url.empty()) url = findTagAttr("<enclosure", "href");
    if (url.empty()) url = findTagAttr("<media:content", "url");
    if (url.empty()) url = findTagAttr("<link", "href", "rel", "enclosure");
    return url;
}

// Extracts an alternate RSS/Atom <link> href from an HTML document when a user
// pastes a podcast website URL instead of the raw XML feed URL.
std::wstring ExtractHtmlFeedLink(const std::string& htmlBody) {
    const std::string lower = ToLowerAsciiCopy(htmlBody);
    size_t searchFrom = 0;
    while (searchFrom < lower.size()) {
        size_t open = lower.find("<link", searchFrom);
        if (open == std::string::npos) break;
        size_t close = lower.find('>', open + 5);
        if (close == std::string::npos) break;
        const std::string tag = htmlBody.substr(open, close - open + 1);
        const std::string type = ToLowerAsciiCopy(Trim(ExtractXmlAttribute(tag, "type")));
        if (type == "application/rss+xml" || type == "application/atom+xml" || type == "text/xml") {
            const std::string href = Trim(ExtractXmlAttribute(tag, "href"));
            if (!href.empty()) return Utf8ToUtf16(href);
        }
        searchFrom = close + 1;
    }
    return L"";
}

// Extracts an Apple Podcasts numeric ID from podcasts.apple.com / itunes.apple.com URLs.
std::wstring ExtractApplePodcastId(const std::wstring& url) {
    std::wstring lower = url;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(::towlower(c));
    if (lower.find(L"podcasts.apple.com/") == std::wstring::npos &&
        lower.find(L"itunes.apple.com/") == std::wstring::npos) {
        return L"";
    }
    size_t idPos = lower.find(L"/id");
    if (idPos == std::wstring::npos) idPos = lower.find(L"id=");
    if (idPos == std::wstring::npos) return L"";
    idPos += 3;
    size_t endPos = idPos;
    while (endPos < lower.size() && lower[endPos] >= L'0' && lower[endPos] <= L'9') ++endPos;
    if (endPos == idPos) return L"";
    return url.substr(idPos, endPos - idPos);
}

} // namespace

std::wstring NormalizePodcastFeedUrl(const std::wstring& rawUrl) {
    auto trimW = [](const std::wstring& s) {
        const size_t b = s.find_first_not_of(L" \t\r\n\"'");
        if (b == std::wstring::npos) return std::wstring();
        const size_t e = s.find_last_not_of(L" \t\r\n\"'");
        return s.substr(b, e - b + 1);
    };
    std::wstring url = trimW(rawUrl);
    if (url.empty()) return L"";
    auto startsWithInsensitive = [](const std::wstring& s, const wchar_t* prefix) {
        const size_t n = std::wcslen(prefix);
        if (s.size() < n) return false;
        for (size_t i = 0; i < n; ++i) {
            if (::towlower(s[i]) != ::towlower(prefix[i])) return false;
        }
        return true;
    };
    // Unwrap feed:https://, podcast:https://, pcast:https://, itpc:https://
    for (const wchar_t* wrapper : { L"feed:", L"podcast:", L"pcast:", L"itpc:" }) {
        if (startsWithInsensitive(url, wrapper)) {
            std::wstring rest = trimW(url.substr(std::wcslen(wrapper)));
            if (startsWithInsensitive(rest, L"//")) {
                url = L"https:" + rest;
            } else {
                url = rest;
            }
            break;
        }
    }
    if (startsWithInsensitive(url, L"https://")) {
        url.replace(0, 8, L"https://");
    } else if (startsWithInsensitive(url, L"http://")) {
        url.replace(0, 7, L"http://");
    } else if (url.find(L"://") == std::wstring::npos && url.find(L'.') != std::wstring::npos) {
        url = L"https://" + url;
    } else {
        return L"";
    }
    if ((url.rfind(L"https://", 0) == 0 && url.size() <= 8) ||
        (url.rfind(L"http://", 0) == 0 && url.size() <= 7)) {
        return L"";
    }
    for (wchar_t c : url) {
        if (c <= 32 || c == 127) return L"";
    }
    return url;
}

std::vector<PodcastImportEntry> ParseOpmlFeeds(const std::string& utf8Xml, size_t maxFeeds) {
    std::vector<PodcastImportEntry> entries;
    if (utf8Xml.empty()) return entries;
    // A flat scan over <outline ...> opening tags covers nested folder trees:
    // every entry outline carries its xmlUrl on the tag itself, whatever the
    // nesting depth. Tag case and attribute quoting are handled by the shared
    // XmlLite helpers.
    const std::string lower = XmlLite::ToLowerAsciiCopy(utf8Xml);
    size_t searchFrom = 0;
    while (entries.size() < maxFeeds) {
        const size_t open = lower.find("<outline", searchFrom);
        if (open == std::string::npos) break;
        searchFrom = open + 8;
        const size_t tagEnd = utf8Xml.find('>', open);
        if (tagEnd == std::string::npos) break;
        const std::string tag = utf8Xml.substr(open, tagEnd - open + 1);
        PodcastImportEntry entry;
        entry.url = Utf8ToUtf16(XmlLite::ExtractXmlAttribute(tag, "xmlUrl"));
        entry.title = Utf8ToUtf16(XmlLite::ExtractXmlAttribute(tag, "text"));
        if (entry.title.empty()) entry.title = Utf8ToUtf16(XmlLite::ExtractXmlAttribute(tag, "title"));
        const std::wstring normalized = NormalizePodcastFeedUrl(entry.url);
        if (normalized.empty()) continue;
        entry.url = normalized;
        if (entry.title.empty()) entry.title = entry.url;
        bool duplicate = false;
        for (const auto& existing : entries) {
            if (existing.url == entry.url) { duplicate = true; break; }
        }
        if (!duplicate) entries.push_back(std::move(entry));
    }
    return entries;
}

std::vector<std::wstring> ParseUrlListText(const std::string& utf8Text, size_t maxUrls) {
    std::vector<std::wstring> urls;
    size_t lineStart = 0;
    while (lineStart <= utf8Text.size() && urls.size() < maxUrls) {
        size_t lineEnd = utf8Text.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = utf8Text.size();
        std::string line = XmlLite::Trim(utf8Text.substr(lineStart, lineEnd - lineStart));
        lineStart = lineEnd + 1;
        if (line.empty() || line[0] == '#') continue;
        const std::wstring normalized = NormalizePodcastFeedUrl(Utf8ToUtf16(line));
        if (normalized.empty()) continue;
        bool duplicate = false;
        for (const auto& existing : urls) {
            if (existing == normalized) { duplicate = true; break; }
        }
        if (!duplicate) urls.push_back(normalized);
    }
    return urls;
}

std::wstring PodcastRefreshService::GetCachePath(const std::wstring& feedUrl, const std::wstring& root) {
    std::wstring cacheDir = root + L"\\cache\\podcasts";
    DWORD attr = GetFileAttributesW(cacheDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        SHCreateDirectoryExW(NULL, cacheDir.c_str(), NULL);
    }
    // Sanitized file name: feed URLs reduce to alnum/dot/dash/underscore, capped
    // at 80 characters, with an FNV-1a suffix so distinct URLs cannot collide.
    std::wstring name;
    for (wchar_t c : feedUrl) {
        const bool safe = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || c == L'.' || c == L'-' || c == L'_';
        if (safe) name += c;
        else if (c >= L'A' && c <= L'Z') name += static_cast<wchar_t>(c - L'A' + L'a');
        else name += L'_';
        if (name.size() >= 80) break;
    }
    if (name.empty()) name = L"feed";
    name += L"-" + std::to_wstring(Fnv1a64(feedUrl.data(), feedUrl.size() * sizeof(wchar_t)));
    return cacheDir + L"\\" + name + L".xml";
}

bool PodcastRefreshService::LoadPodcastCache(const std::wstring& path, std::string& outBody, bool& outFresh) {
    return LoadPodcastCacheImpl(path, outBody, outFresh);
}

StorageStatus PodcastRefreshService::SavePodcastCache(const std::wstring& path, const std::string& body, int64_t nowSec) {
    return SavePodcastCacheImpl(path, body, nowSec);
}

bool PodcastRefreshService::AnyCacheStale(const std::vector<std::wstring>& feedUrls, const std::wstring& root) {
    // Timestamp-only probe (no parse): a cache written by SavePodcastCacheImpl
    // has its last-write time equal to the embedded cachedAtSec stamp.
    SYSTEMTIME utcNow{};
    GetSystemTime(&utcNow);
    FILETIME nowFt{};
    if (!SystemTimeToFileTime(&utcNow, &nowFt)) return true;
    const int64_t nowSec = (static_cast<int64_t>(nowFt.dwHighDateTime) << 32 | nowFt.dwLowDateTime) / 10000000LL;
    for (const std::wstring& url : feedUrls) {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(GetCachePath(url, root).c_str(), GetFileExInfoStandard, &info)) return true;
        const int64_t writtenSec =
            (static_cast<int64_t>(info.ftLastWriteTime.dwHighDateTime) << 32 | info.ftLastWriteTime.dwLowDateTime) / 10000000LL;
        if (nowSec - writtenSec >= kCacheFreshnessSec) return true;
    }
    return false;
}

PodcastAddPreviewResult PodcastRefreshService::FetchSingleFeed(const std::wstring& rawFeedUrl, const std::wstring& root,
    IHttpTransport* customTransport, std::shared_ptr<OperationCancellation> cancellation) {
    PodcastAddPreviewResult result;
    std::wstring feedUrl = NormalizePodcastFeedUrl(rawFeedUrl);
    if (feedUrl.empty()) {
        result.errorMessage = L"Only http:// or https:// podcast feed URLs are supported";
        return result;
    }
    WinHttpTransport defaultTransport;
    IHttpTransport* transport = customTransport ? customTransport : &defaultTransport;

    HttpRequestOptions options;
    options.maxResponseBytes = kMaxResponseBytes;
    options.timeoutMs = kFetchTimeoutMs;
    options.maxRedirects = 8;
    options.allowHttp = true;
    options.cancellation = cancellation;

    // Resolve Apple Podcasts directory URLs (podcasts.apple.com/.../id<digits>) to their underlying RSS feed URL.
    const std::wstring appleId = ExtractApplePodcastId(feedUrl);
    if (!appleId.empty()) {
        HttpRequestOptions lookupOpts = options;
        lookupOpts.maxResponseBytes = 2 * 1024 * 1024;
        HttpResponse lookupRes = transport->Get(L"https://itunes.apple.com/lookup?id=" + appleId + L"&entity=podcast", lookupOpts);
        if (lookupRes.success && !lookupRes.body.empty()) {
            JsonValue json;
            if (JsonValue::parse(lookupRes.body, json) && json.is_object() && json.has_key("results") && json.get("results").is_array()) {
                const auto& arr = json.get("results").as_array();
                for (const auto& entry : arr) {
                    if (entry.is_object() && entry.has_key("feedUrl") && entry.get("feedUrl").is_string()) {
                        std::wstring resolved = NormalizePodcastFeedUrl(Utf8ToUtf16(entry.get("feedUrl").as_string()));
                        if (!resolved.empty()) { feedUrl = resolved; break; }
                    }
                }
            }
        }
    }

    const std::wstring cachePath = GetCachePath(feedUrl, root);
    std::string cachedBody;
    bool cacheFresh = false;
    const bool haveCache = LoadPodcastCacheImpl(cachePath, cachedBody, cacheFresh);
    if (haveCache && cacheFresh) {
        PodcastFeed feed = ParseRssFeed(cachedBody, feedUrl);
        if (!feed.title.empty() && !feed.episodes.empty()) {
            result.feed = std::move(feed);
            result.success = true;
            return result;
        }
    }

    HttpResponse response = transport->Get(feedUrl, options);
    if (response.success && !response.body.empty()) {
        PodcastFeed feed = ParseRssFeed(response.body, feedUrl);
        if (feed.episodes.empty()) {
            const std::wstring altFeed = NormalizePodcastFeedUrl(ExtractHtmlFeedLink(response.body));
            if (!altFeed.empty() && altFeed != feedUrl) {
                HttpResponse altRes = transport->Get(altFeed, options);
                if (altRes.success && !altRes.body.empty()) {
                    feed = ParseRssFeed(altRes.body, altFeed);
                    if (!feed.episodes.empty()) {
                        feedUrl = altFeed;
                        response = std::move(altRes);
                    }
                }
            }
        }
        if (!feed.title.empty() && !feed.episodes.empty()) {
            SavePodcastCacheImpl(GetCachePath(feedUrl, root), response.body, time(nullptr));
            result.feed = std::move(feed);
            result.success = true;
            return result;
        }
    }

    if (haveCache) {
        PodcastFeed feed = ParseRssFeed(cachedBody, feedUrl);
        if (!feed.title.empty() && !feed.episodes.empty()) {
            result.feed = std::move(feed);
            result.success = true;
            return result;
        }
    }

    result.errorMessage = response.errorMessage.empty() ? L"the URL is not a valid RSS podcast feed" : response.errorMessage;
    return result;
}

PodcastRefreshResult PodcastRefreshService::FetchFeeds(const std::vector<std::wstring>& feedUrls, const std::wstring& root,
    IHttpTransport* customTransport, std::shared_ptr<OperationCancellation> cancellation) {
    PodcastRefreshResult result;
    for (const std::wstring& feedUrl : feedUrls) {
        if (cancellation && cancellation->IsCancelled()) break;
        PodcastAddPreviewResult single = FetchSingleFeed(feedUrl, root, customTransport, cancellation);
        if (single.success) {
            result.feeds.push_back(std::move(single.feed));
        } else {
            result.failures.push_back({ feedUrl, single.errorMessage.empty() ? L"Podcast feed unavailable" : single.errorMessage });
        }
    }
    result.success = !result.feeds.empty();
    return result;
}

PodcastFeed ParseRssFeed(const std::string& utf8Xml, const std::wstring& feedUrl, size_t maxEpisodes) {
    PodcastFeed feed;
    feed.url = feedUrl;
    if (utf8Xml.empty()) return feed;
    const std::string lower = ToLowerAsciiCopy(utf8Xml);
    const size_t channelOpen = lower.find("<channel");
    if (channelOpen == std::string::npos) return feed; // Not RSS channel markup: empty feed.
    // Channel <title>: the first <title> inside the channel header, before the
    // first <item> (item titles must not win).
    size_t headerEnd = lower.find("<item", channelOpen);
    if (headerEnd == std::string::npos) headerEnd = lower.find("</channel", channelOpen);
    const size_t headerLength = headerEnd == std::string::npos ? std::string::npos : headerEnd - channelOpen;
    const std::string headerBlock = utf8Xml.substr(channelOpen, headerLength);
    feed.title = Utf8ToUtf16(Trim(ExtractXmlElement(headerBlock, "title")));
    if (feed.title.empty()) feed.title = Utf8ToUtf16(Trim(ExtractXmlElement(headerBlock, "itunes:author")));
    // Items: <item ...> ... </item> blocks, capped at maxEpisodes.
    size_t cursor = channelOpen;
    while (feed.episodes.size() < maxEpisodes) {
        const size_t itemOpen = lower.find("<item", cursor);
        if (itemOpen == std::string::npos) break;
        // Reject longer tag names sharing the prefix (e.g. <items>).
        const size_t afterOpen = itemOpen + 5;
        if (afterOpen < lower.size() && lower[afterOpen] != '>' && lower[afterOpen] != ' ' && lower[afterOpen] != '\t' &&
            lower[afterOpen] != '\r' && lower[afterOpen] != '\n' && lower[afterOpen] != '/') {
            cursor = itemOpen + 5;
            continue;
        }
        const size_t itemClose = lower.find("</item", afterOpen);
        if (itemClose == std::string::npos) break;
        cursor = lower.find('>', itemClose);
        if (cursor == std::string::npos) break;
        ++cursor;
        const std::string itemBlock = utf8Xml.substr(itemOpen, cursor - itemOpen);
        std::string enclosureUrl = ExtractEnclosureUrl(itemBlock);
        const std::string lowerEnc = ToLowerAsciiCopy(enclosureUrl);
        if (lowerEnc.rfind("http://", 0) != 0 && lowerEnc.rfind("https://", 0) != 0) continue; // Skip items without an http(s) enclosure.
        if (enclosureUrl.rfind("HTTPS://", 0) == 0 || lowerEnc.rfind("https://", 0) == 0) {
            enclosureUrl.replace(0, 8, "https://");
        } else if (lowerEnc.rfind("http://", 0) == 0) {
            enclosureUrl.replace(0, 7, "http://");
        }
        PodcastEpisode episode;
        episode.url = Utf8ToUtf16(enclosureUrl);
        episode.title = Utf8ToUtf16(Trim(ExtractXmlElement(itemBlock, "title")));
        if (episode.title.empty()) episode.title = Utf8ToUtf16(Trim(ExtractXmlElement(itemBlock, "itunes:title")));
        episode.publishedSec = ParseRfc822Date(ExtractXmlElement(itemBlock, "pubDate"));
        episode.durationSec = ParseItunesDuration(ExtractXmlElement(itemBlock, "itunes:duration"));
        if (episode.title.empty()) episode.title = episode.url; // Unnamed episodes fall back to their URL.
        feed.episodes.push_back(std::move(episode));
    }
    if (feed.title.empty() && !feed.episodes.empty()) feed.title = feedUrl;
    return feed;
}

bool PodcastSubscriptions::Add(const std::wstring& title, const std::wstring& url) {
    if (url.empty()) return false;
    for (const auto& existing : feeds_) {
        if (existing.url == url) return false; // Dedupe by URL.
    }
    if (feeds_.size() >= kMaxFeeds) return false;
    PodcastFeed feed;
    feed.url = url;
    feed.title = title.empty() ? url : title;
    feeds_.push_back(std::move(feed));
    return true;
}

bool PodcastSubscriptions::Remove(const std::wstring& url) {
    const auto it = std::remove_if(feeds_.begin(), feeds_.end(),
        [&url](const PodcastFeed& feed) { return feed.url == url; });
    if (it == feeds_.end()) return false;
    feeds_.erase(it, feeds_.end());
    return true;
}

StorageStatus PodcastSubscriptions::Load(const std::wstring& dataDir, const std::wstring& fileName) {
    const std::wstring fullPath = dataDir + L"\\" + fileName;
    std::string jsonContent;
    StorageStatus readStatus = ReadTextFile(fullPath, jsonContent);
    if (!readStatus.is_ok()) {
        // A missing subscriptions file is the pristine state, not an error.
        DWORD fileAttr = GetFileAttributesW(fullPath.c_str());
        if (fileAttr == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
            feeds_.clear();
            return StorageStatus::Ok(fullPath);
        }
        return readStatus;
    }
    JsonValue root;
    if (!JsonValue::parse(jsonContent, root)) {
        return StorageStatus::Error(StorageErrorCode::SerializationFailed, 0, fullPath, "JSON parse error");
    }
    if (!root.is_object()) {
        return StorageStatus::Error(StorageErrorCode::SerializationFailed, 0, fullPath, "Root must be an object");
    }
    const JsonValue& arr = root.get("feeds");
    if (arr.is_array()) {
        feeds_.clear();
        const auto& values = arr.as_array();
        for (size_t i = 0; i < values.size() && feeds_.size() < kMaxFeeds; ++i) {
            const JsonValue& item = values[i];
            if (!item.is_object()) continue; // Unknown/malformed fields are tolerated and skipped.
            PodcastFeed feed;
            feed.url = Utf8ToUtf16(item.get("url").as_string(""));
            feed.title = Utf8ToUtf16(item.get("title").as_string(""));
            if (feed.url.empty()) continue;
            if (feed.title.empty()) feed.title = feed.url;
            feeds_.push_back(std::move(feed));
        }
    }
    return StorageStatus::Ok(fullPath);
}

StorageStatus PodcastSubscriptions::Save(const std::wstring& dataDir, const std::wstring& fileName) {
    const std::wstring fullPath = dataDir + L"\\" + fileName;
    JsonValue root = JsonValue::Object();
    root["schemaVersion"] = JsonValue(static_cast<int64_t>(kCurrentSubscriptionsSchemaVersion));
    JsonValue arr = JsonValue::Array();
    for (const auto& feed : feeds_) {
        JsonValue obj = JsonValue::Object();
        obj["url"] = JsonValue(Utf16ToUtf8(feed.url));
        obj["title"] = JsonValue(Utf16ToUtf8(feed.title));
        arr.push_back(obj);
    }
    root["feeds"] = arr;
    return AtomicWriteTextFile(fullPath, root.stringify(true));
}

} // namespace Aperture
