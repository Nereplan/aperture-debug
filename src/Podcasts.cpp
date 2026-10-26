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
    bool ok = GetFileSizeEx(hFile, &size) && size.QuadPart > 0 && size.QuadPart <= 16 * 1024 * 1024;
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
// then read its url attribute.
std::string ExtractEnclosureUrl(const std::string& itemBlock) {
    const std::string lower = ToLowerAsciiCopy(itemBlock);
    size_t searchFrom = 0;
    while (searchFrom < lower.size()) {
        size_t open = lower.find("<enclosure", searchFrom);
        if (open == std::string::npos) return "";
        size_t close = lower.find('>', open);
        if (close == std::string::npos) return "";
        const std::string tag = itemBlock.substr(open, close - open + 1);
        const std::string url = Trim(ExtractXmlAttribute(tag, "url"));
        if (!url.empty()) return url;
        searchFrom = close + 1; // Tolerate malformed enclosures without a url attribute.
    }
    return "";
}

} // namespace

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

PodcastRefreshResult PodcastRefreshService::FetchFeeds(const std::vector<std::wstring>& feedUrls, const std::wstring& root,
    IHttpTransport* customTransport, std::shared_ptr<OperationCancellation> cancellation) {
    PodcastRefreshResult result;
    WinHttpTransport defaultTransport;
    IHttpTransport* transport = customTransport ? customTransport : &defaultTransport;
    for (const std::wstring& feedUrl : feedUrls) {
        if (cancellation && cancellation->IsCancelled()) break;
        // http(s) only; other schemes fail up front with a per-feed error.
        if (feedUrl.rfind(L"https://", 0) != 0 && feedUrl.rfind(L"http://", 0) != 0) {
            result.failures.push_back({ feedUrl, L"Only http:// or https:// podcast feed URLs are supported" });
            continue;
        }
        const std::wstring cachePath = GetCachePath(feedUrl, root);
        std::string cachedBody;
        bool cacheFresh = false;
        const bool haveCache = LoadPodcastCacheImpl(cachePath, cachedBody, cacheFresh);
        if (haveCache && cacheFresh) {
            PodcastFeed feed = ParseRssFeed(cachedBody, feedUrl);
            if (!feed.title.empty() || !feed.episodes.empty()) {
                result.feeds.push_back(std::move(feed));
                continue;
            }
        }
        HttpRequestOptions options;
        options.maxResponseBytes = kMaxResponseBytes;
        options.timeoutMs = kFetchTimeoutMs;
        options.maxRedirects = 3;
        options.cancellation = cancellation;
        HttpResponse response = transport->Get(feedUrl, options);
        if (response.success && !response.body.empty()) {
            PodcastFeed feed = ParseRssFeed(response.body, feedUrl);
            if (!feed.title.empty() || !feed.episodes.empty()) {
                SavePodcastCacheImpl(cachePath, response.body, time(nullptr));
                result.feeds.push_back(std::move(feed));
                continue;
            }
        }
        // Network refresh failed: fall back to the stale cache when one exists.
        if (haveCache) {
            PodcastFeed feed = ParseRssFeed(cachedBody, feedUrl);
            if (!feed.title.empty() || !feed.episodes.empty()) {
                result.feeds.push_back(std::move(feed));
                continue;
            }
        }
        result.failures.push_back({ feedUrl, response.errorMessage.empty() ? L"Podcast feed unavailable" : response.errorMessage });
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
    feed.title = Utf8ToUtf16(Trim(ExtractXmlElement(utf8Xml.substr(channelOpen, headerLength), "title")));
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
        if (enclosureUrl.rfind("http://", 0) != 0 && enclosureUrl.rfind("https://", 0) != 0) continue; // Skip items without an http(s) enclosure.
        PodcastEpisode episode;
        episode.url = Utf8ToUtf16(enclosureUrl);
        episode.title = Utf8ToUtf16(Trim(ExtractXmlElement(itemBlock, "title")));
        episode.publishedSec = ParseRfc822Date(ExtractXmlElement(itemBlock, "pubDate"));
        episode.durationSec = ParseItunesDuration(ExtractXmlElement(itemBlock, "itunes:duration"));
        if (episode.title.empty()) episode.title = episode.url; // Unnamed episodes fall back to their URL.
        feed.episodes.push_back(std::move(episode));
    }
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
