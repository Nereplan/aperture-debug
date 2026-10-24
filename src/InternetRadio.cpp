#include "InternetRadio.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <ctime>
#include <set>

namespace Aperture {

namespace {

std::wstring TrimWhitespace(const std::wstring& text) {
    size_t begin = 0, end = text.size();
    while (begin < end && std::iswspace(text[begin])) ++begin;
    while (end > begin && std::iswspace(text[end - 1])) --end;
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

} // namespace Aperture
