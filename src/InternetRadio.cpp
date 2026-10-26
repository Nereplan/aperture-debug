#include "InternetRadio.h"
#include "Common.h"
#include "PathFileHelpers.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <ctime>
#include <map>
#include <set>

namespace Aperture {

namespace {

std::wstring TrimWhitespace(const std::wstring& text) {
    size_t begin = 0, end = text.size();
    while (begin < end && std::iswspace(text[begin])) ++begin;
    while (end > begin && std::iswspace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::string ToLowerAscii(const std::string& text) {
    std::string out = text;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string TrimAscii(const std::string& text) {
    size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

// The cache file wraps the raw response body with a wall-clock freshness stamp:
// { "cachedAtSec": <unix time>, "body": "<raw stations JSON>" }.
bool LoadRadioCache(const std::wstring& path, std::string& outBody, bool& outFresh) {
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
    outFresh = (time(nullptr) - cachedAt) < InternetRadioService::kCacheFreshnessSec;
    return true;
}

StorageStatus SaveRadioCache(const std::wstring& path, const std::string& body, int64_t nowSec) {
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
        return StorageStatus::Error(StorageErrorCode::FileOpenFailed, err, tmpPath, "Failed to create temporary radio cache file");
    }
    DWORD written = 0;
    BOOL writeOk = WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()), &written, NULL);
    writeOk = writeOk && (written == static_cast<DWORD>(content.size()));
    DWORD writeErr = writeOk ? 0 : GetLastError();
    if (writeOk) { FlushFileBuffers(hFile); }
    CloseHandle(hFile);
    if (!writeOk) {
        DeleteFileW(tmpPath.c_str());
        return StorageStatus::Error(StorageErrorCode::FileWriteFailed, writeErr, tmpPath, "Failed to write radio cache data");
    }
    if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD moveErr = GetLastError();
        DeleteFileW(tmpPath.c_str());
        return StorageStatus::Error(StorageErrorCode::AtomicRenameFailed, moveErr, path, "Failed to atomically rename radio cache file");
    }
    return StorageStatus::Ok(path);
}

} // namespace

std::wstring InternetRadioService::GetCachePath(const std::wstring& root) {
    std::wstring cacheDir = root + L"\\cache\\radio";
    DWORD attr = GetFileAttributesW(cacheDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        SHCreateDirectoryExW(NULL, cacheDir.c_str(), NULL);
    }
    return cacheDir + L"\\stations.json";
}

std::vector<RadioStation> ParseRadioBrowserJson(const std::string& json, size_t maxStations) {
    std::vector<RadioStation> stations;
    if (json.empty()) return stations;
    JsonValue root;
    if (!JsonValue::parse(json, root) || !root.is_array()) return stations;
    std::set<std::wstring> seenUrls;
    for (const auto& item : root.as_array()) {
        if (stations.size() >= maxStations) break;
        if (!item.is_object()) continue;
        std::string url = item.get("url_resolved").as_string(item.get("url").as_string(""));
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) continue;
        std::wstring urlWide = Utf8ToUtf16(url);
        if (!seenUrls.insert(urlWide).second) continue; // dedupe by URL
        RadioStation station;
        station.name = TrimWhitespace(item.get("name").as_wstring(L""));
        if (station.name.empty()) station.name = L"Unnamed station";
        if (station.name.length() > 64) station.name = station.name.substr(0, 64);
        station.url = std::move(urlWide);
        station.bitrateKbps = item.get("bitrate").as_int(0);
        station.codec = TrimWhitespace(item.get("codec").as_wstring(L""));
        station.country = TrimWhitespace(item.get("country").as_wstring(L""));
        stations.push_back(std::move(station));
    }
    return stations;
}

RadioDirectoryResult InternetRadioService::FetchDirectory( const std::wstring& root, IHttpTransport* customTransport,
    std::shared_ptr<OperationCancellation> cancellation) {
    RadioDirectoryResult result;
    const std::wstring cachePath = GetCachePath(root);
    std::string cachedBody;
    bool cacheFresh = false;
    const bool haveCache = LoadRadioCache(cachePath, cachedBody, cacheFresh);
    if (haveCache && cacheFresh) {
        result.success = true;
        result.fromCache = true;
        result.stations = ParseRadioBrowserJson(cachedBody);
        return result;
    }
    WinHttpTransport defaultTransport;
    IHttpTransport* transport = customTransport ? customTransport : &defaultTransport;
    HttpRequestOptions options;
    options.maxResponseBytes = kMaxResponseBytes;
    options.timeoutMs = kFetchTimeoutMs;
    options.maxRedirects = 3;
    options.cancellation = cancellation;
    HttpResponse response = transport->Get(kDirectoryUrl, options);
    if (response.success && !response.body.empty()) {
        std::vector<RadioStation> stations = ParseRadioBrowserJson(response.body);
        if (!stations.empty()) {
            SaveRadioCache(cachePath, response.body, time(nullptr));
            result.success = true;
            result.stations = std::move(stations);
            return result;
        }
    }
    // Network refresh failed: fall back to the stale cache when one exists.
    if (haveCache) {
        std::vector<RadioStation> stations = ParseRadioBrowserJson(cachedBody);
        if (!stations.empty()) {
            result.success = true;
            result.fromCache = true;
            result.stale = true;
            result.stations = std::move(stations);
            return result;
        }
    }
    result.success = false;
    result.errorMessage = response.errorMessage.empty() ? L"Radio directory unavailable" : response.errorMessage;
    return result;
}

std::wstring InternetRadioService::BuildSearchUrl(const std::wstring& query) {
    // radio-browser matches station names case-insensitively; order by votes so
    // established stations surface first, mirroring the directory's popularity
    // ordering.
    const std::string encoded = PercentEncodeUrlPathUtf8(query);
    return L"https://de1.api.radio-browser.info/json/stations/byname/" +
        Utf8ToUtf16(encoded) + L"?limit=60&order=votes&reverse=true&hidebroken=true";
}

RadioSearchResult InternetRadioService::SearchStations( const std::wstring& query,
    IHttpTransport* customTransport, std::shared_ptr<OperationCancellation> cancellation) {
    RadioSearchResult result;
    result.query = query;
    if (query.empty()) return result;
    WinHttpTransport defaultTransport;
    IHttpTransport* transport = customTransport ? customTransport : &defaultTransport;
    HttpRequestOptions options;
    options.maxResponseBytes = kMaxResponseBytes;
    options.timeoutMs = kFetchTimeoutMs;
    options.maxRedirects = 3;
    options.cancellation = cancellation;
    HttpResponse response = transport->Get(BuildSearchUrl(query), options);
    if (response.success && !response.body.empty()) {
        std::vector<RadioStation> stations = ParseRadioBrowserJson(response.body);
        if (!stations.empty()) {
            result.success = true;
            result.stations = std::move(stations);
            return result;
        }
        // A valid empty array means no matches, not a failure.
        JsonValue root;
        if (JsonValue::parse(response.body, root) && root.is_array()) {
            result.success = true;
            return result;
        }
    }
    result.success = false;
    result.errorMessage = response.errorMessage.empty() ? L"Station search unavailable" : response.errorMessage;
    return result;
}

std::string PercentEncodeUrlPathUtf8(const std::wstring& text) {
    // Unreserved characters per RFC 3986 pass through; everything else is
    // emitted as its UTF-8 byte sequence in %XX form.
    static const char* kHex = "0123456789ABCDEF";
    const std::string utf8 = Utf16ToUtf8(text);
    std::string out;
    out.reserve(utf8.size() + utf8.size() / 2);
    for (const char raw : utf8) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (unreserved) {
            out.push_back(raw);
        } else {
            out.push_back('%');
            out.push_back(kHex[ch >> 4]);
            out.push_back(kHex[ch & 0xF]);
        }
    }
    return out;
}

std::vector<RadioStation> ParseRadioPlaylistText(const std::string& utf8Text, size_t maxStations) {
    std::vector<RadioStation> stations;
    if (utf8Text.empty()) return stations;
    const auto addStation = [&](std::wstring url, std::wstring name) {
        if (stations.size() >= maxStations) return;
        // Trim whitespace/quotes the way the directory parser and the podcast
        // normalizer do; M3U exports commonly wrap names in quotes.
        const auto trimW = [](const std::wstring& s) {
            const size_t b = s.find_first_not_of(L" \t\r\n\"'");
            if (b == std::wstring::npos) return std::wstring();
            const size_t e = s.find_last_not_of(L" \t\r\n\"'");
            return s.substr(b, e - b + 1);
        };
        url = trimW(std::move(url));
        if (url.rfind(L"http://", 0) != 0 && url.rfind(L"https://", 0) != 0) return;
        for (size_t i = 0; i < stations.size(); ++i) {
            if (stations[i].url == url) return;
        }
        RadioStation station;
        station.url = std::move(url);
        station.name = trimW(std::move(name));
        if (station.name.empty()) station.name = L"Unnamed station";
        if (station.name.length() > 64) station.name = station.name.substr(0, 64);
        stations.push_back(std::move(station));
    };
    const std::string lower = ToLowerAscii(utf8Text);
    if (lower.find("[playlist]") != std::string::npos) {
        // PLS: FileN=... entries with TitleN=... names on separate lines, so
        // collect the titles first and then walk the file entries in order.
        std::map<int, std::string> titles;
        size_t lineStart = 0;
        while (lineStart <= utf8Text.size()) {
            size_t lineEnd = utf8Text.find('\n', lineStart);
            if (lineEnd == std::string::npos) lineEnd = utf8Text.size();
            std::string line = TrimAscii(utf8Text.substr(lineStart, lineEnd - lineStart));
            lineStart = lineEnd + 1;
            if (line.empty()) continue;
            const std::string lowerLine = ToLowerAscii(line);
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            if (lowerLine.rfind("title", 0) == 0) {
                try { titles[std::stoi(lowerLine.substr(5, eq - 5))] = TrimAscii(line.substr(eq + 1)); } catch (...) {}
            }
        }
        lineStart = 0;
        while (lineStart <= utf8Text.size()) {
            size_t lineEnd = utf8Text.find('\n', lineStart);
            if (lineEnd == std::string::npos) lineEnd = utf8Text.size();
            std::string line = TrimAscii(utf8Text.substr(lineStart, lineEnd - lineStart));
            lineStart = lineEnd + 1;
            if (line.empty()) continue;
            const std::string lowerLine = ToLowerAscii(line);
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            if (lowerLine.rfind("file", 0) != 0) continue;
            int index = -1;
            try { index = std::stoi(lowerLine.substr(4, eq - 4)); } catch (...) {}
            if (index < 0) continue;
            addStation(Utf8ToUtf16(TrimAscii(line.substr(eq + 1))), Utf8ToUtf16(titles[index]));
        }
        return stations;
    }
    // M3U/M3U8 and plain text: an #EXTINF line names the URL that follows.
    std::wstring pendingName;
    size_t lineStart = 0;
    while (lineStart <= utf8Text.size()) {
        size_t lineEnd = utf8Text.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = utf8Text.size();
        std::string line = TrimAscii(utf8Text.substr(lineStart, lineEnd - lineStart));
        lineStart = lineEnd + 1;
        if (line.empty() || line[0] == '#') {
            if (line.rfind("#EXTINF", 0) == 0) {
                const size_t comma = line.find(',');
                pendingName = (comma == std::string::npos) ? std::wstring()
                    : Utf8ToUtf16(TrimAscii(line.substr(comma + 1)));
            }
            continue;
        }
        addStation(Utf8ToUtf16(line), std::move(pendingName));
        pendingName.clear();
    }
    return stations;
}

bool RadioFavorites::Add(const RadioStation& station) {
    if (station.url.empty()) return false;
    if (Contains(station.url)) return false;
    if (stations_.size() >= kMaxStations) return false;
    RadioStation copy = station;
    copy.name = TrimWhitespace(copy.name);
    if (copy.name.empty()) copy.name = L"Unnamed station";
    stations_.push_back(std::move(copy));
    return true;
}

bool RadioFavorites::Remove(const std::wstring& url) {
    for (size_t i = 0; i < stations_.size(); ++i) {
        if (stations_[i].url == url) {
            stations_.erase(stations_.begin() + static_cast<ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

bool RadioFavorites::Contains(const std::wstring& url) const {
    for (const auto& station : stations_) {
        if (station.url == url) return true;
    }
    return false;
}

StorageStatus RadioFavorites::Load(const std::wstring& dataDir, const std::wstring& fileName) {
    const std::wstring fullPath = dataDir + L"\\" + fileName;
    std::string jsonContent;
    StorageStatus readStatus = ReadTextFile(fullPath, jsonContent);
    if (!readStatus.is_ok()) {
        // A missing favorites file is the pristine state, not an error.
        DWORD fileAttr = GetFileAttributesW(fullPath.c_str());
        if (fileAttr == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
            stations_.clear();
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
    const JsonValue& arr = root.get("stations");
    if (arr.is_array()) {
        stations_.clear();
        const auto& values = arr.as_array();
        for (size_t i = 0; i < values.size() && stations_.size() < kMaxStations; ++i) {
            const JsonValue& item = values[i];
            if (!item.is_object()) continue; // Unknown/malformed fields are tolerated and skipped.
            RadioStation station;
            station.url = Utf8ToUtf16(item.get("url").as_string(""));
            station.name = Utf8ToUtf16(item.get("name").as_string(""));
            station.bitrateKbps = static_cast<int>(item.get("bitrateKbps").as_int64(0));
            station.codec = Utf8ToUtf16(item.get("codec").as_string(""));
            station.country = Utf8ToUtf16(item.get("country").as_string(""));
            if (station.url.empty()) continue;
            if (station.name.empty()) station.name = L"Unnamed station";
            if (!Contains(station.url)) stations_.push_back(std::move(station));
        }
    }
    return StorageStatus::Ok(fullPath);
}

StorageStatus RadioFavorites::Save(const std::wstring& dataDir, const std::wstring& fileName) {
    const std::wstring fullPath = dataDir + L"\\" + fileName;
    JsonValue root = JsonValue::Object();
    root["schemaVersion"] = JsonValue(static_cast<int64_t>(kCurrentSchemaVersion));
    JsonValue arr = JsonValue::Array();
    for (const auto& station : stations_) {
        JsonValue obj = JsonValue::Object();
        obj["name"] = JsonValue(Utf16ToUtf8(station.name));
        obj["url"] = JsonValue(Utf16ToUtf8(station.url));
        obj["bitrateKbps"] = JsonValue(static_cast<int64_t>(station.bitrateKbps));
        obj["codec"] = JsonValue(Utf16ToUtf8(station.codec));
        obj["country"] = JsonValue(Utf16ToUtf8(station.country));
        arr.push_back(obj);
    }
    root["stations"] = arr;
    return AtomicWriteTextFile(fullPath, root.stringify(true));
}

} // namespace Aperture
