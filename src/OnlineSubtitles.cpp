#include "OnlineSubtitles.h"
#include <windows.h>
#include <winhttp.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

namespace Aperture {

// --- OnlineSubtitleService Implementation ---

OnlineSubtitleService::OnlineSubtitleService() : defaultTransport_(std::make_unique<WinHttpTransport>()) {}

OnlineSubtitleService& OnlineSubtitleService::Instance() { static OnlineSubtitleService service; return service; }

static OnlineSubtitleStatus MapHttpTransportStatus(HttpTransportStatus s) {
    switch (s) {
        case HttpTransportStatus::Success: return OnlineSubtitleStatus::Success;
        case HttpTransportStatus::InvalidUrl: return OnlineSubtitleStatus::InvalidUrl;
        case HttpTransportStatus::HttpsRequired: return OnlineSubtitleStatus::HttpsRequired;
        case HttpTransportStatus::NetworkError: return OnlineSubtitleStatus::NetworkError;
        case HttpTransportStatus::Timeout: return OnlineSubtitleStatus::Timeout;
        case HttpTransportStatus::TooManyRedirects: return OnlineSubtitleStatus::TooManyRedirects;
        case HttpTransportStatus::ResponseTooLarge: return OnlineSubtitleStatus::ResponseTooLarge;
        case HttpTransportStatus::HttpError: return OnlineSubtitleStatus::HttpError;
        case HttpTransportStatus::Cancelled: return OnlineSubtitleStatus::Cancelled;
        default: return OnlineSubtitleStatus::NetworkError;
    }
}

static bool IsSceneTagOrNoise(const std::wstring& tokenLower) {
    static const wchar_t* const kTags[] = {
        L"1080p", L"720p", L"480p", L"2160p", L"4k", L"8k",
        L"bluray", L"bdrip", L"brrip", L"webrip", L"web-dl", L"webdl", L"hdtv", L"dvdrip", L"hdrip",
        L"x264", L"x265", L"h264", L"h265", L"hevc", L"avc", L"xvid", L"divx",
        L"aac", L"dts", L"ac3", L"eac3", L"mp3", L"flac", L"ddp5", L"atmos", L"truehd",
        L"hd", L"sd", L"uhd", L"hdr", L"sdr", L"hdr10", L"dv",
        L"repack", L"proper", L"internal", L"remux", L"unrated", L"extended", L"director", L"directors",
        L"cut", L"yts", L"rarbg", L"yify", L"eztv", L"ettv", L"rartv", L"sparks", L"geckos"
    };
    for (const auto* tag : kTags) { if (tokenLower == tag) return true; }
    return false;
}

std::wstring OnlineSubtitleService::DeriveSearchQuery(const std::wstring& mediaPath, const std::wstring& overrideTitle) {
    std::wstring raw;
    if (!overrideTitle.empty()) { raw = overrideTitle; } else {
        raw = GetFileNameFromPath(mediaPath);
        // Strip media extension
        size_t dotPos = raw.find_last_of(L'.');
        if (dotPos != std::wstring::npos) {
            std::wstring ext = raw.substr(dotPos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext == L".mp4" || ext == L".mkv" || ext == L".avi" || ext == L".mov" || ext == L".wmv" || ext == L".flv" ||
                ext == L".webm" || ext == L".m4v" || ext == L".ts" || ext == L".m2ts" || ext == L".iso" || ext == L".vob") {
                raw = raw.substr(0, dotPos);
            }
        }
    }
    if (raw.empty()) return L"";
    // Remove bracketed expressions like [1080p], (2024), etc. if they contain noise, but keep content if it looks like a clean title.
    std::wstring sanitized;
    bool inBracket = false;
    for (wchar_t c : raw) {
        if (c == L'[' || c == L'(' || c == L'{') {
            inBracket = true;
            sanitized.push_back(L' ');
        } else if (c == L']' || c == L')' || c == L'}') {
            inBracket = false;
            sanitized.push_back(L' ');
        } else if (c == L'.' || c == L'_' || c == L'-' || c == L'+') {
            sanitized.push_back(L' ');
        } else if (c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|') {
            sanitized.push_back(L' ');
        } else { sanitized.push_back(c); }
    }
    // Tokenize words and strip noise tags
    std::wistringstream stream(sanitized);
    std::wstring token;
    std::vector<std::wstring> cleanTokens;
    while (stream >> token) {
        std::wstring tokenLower = token;
        std::transform(tokenLower.begin(), tokenLower.end(), tokenLower.begin(), ::towlower);
        if (IsSceneTagOrNoise(tokenLower)) { continue; }
        // Filter standalone non-alphanumeric noise
        bool hasAlnum = false;
        for (wchar_t ch : token) {
            if (::iswalnum(ch)) { hasAlnum = true; break; }
        }
        if (hasAlnum) { cleanTokens.push_back(token); }
    }
    std::wstring result;
    for (size_t i = 0; i < cleanTokens.size(); ++i) { if (i > 0) result += L" "; result += cleanTokens[i]; }
    // Limit length
    if (result.length() > kMaxQueryLength) {
        result = result.substr(0, kMaxQueryLength);
        size_t lastSpace = result.find_last_of(L' ');
        if (lastSpace != std::wstring::npos && lastSpace > 20) { result = result.substr(0, lastSpace); }
    }
    // Fallback if query was over-cleaned
    if (result.empty()) { result = GetFileNameFromPath(mediaPath); }
    return result;
}

bool OnlineSubtitleService::IsValidEndpointTemplate(const std::string& endpointTemplate) {
    if (endpointTemplate.empty()) return true; // Default fallback is valid
    // Must start with https://
    if (endpointTemplate.rfind("https://", 0) != 0) { return false; }
    // Must not contain control characters or spaces
    for (char c : endpointTemplate) {
        if (static_cast<unsigned char>(c) <= 32 || c == 127) { return false; }
    }
    return true;
}

std::string OnlineSubtitleService::UrlEncodeUtf8(const std::wstring& text) {
    std::string utf8 = Utf16ToUtf8(text);
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (unsigned char c : utf8) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') { escaped << c; } else if (c == ' ') {
            escaped << "%20";
        } else { escaped << '%' << std::setw(2) << std::uppercase << int(c); }
    }
    return escaped.str();
}

std::wstring OnlineSubtitleService::BuildSearchUrl( const std::string& endpointTemplate, const std::wstring& query, const std::string& lang)
{
    std::string templ = endpointTemplate.empty() ? kDefaultEndpointTemplate : endpointTemplate;
    std::string encodedQuery = UrlEncodeUtf8(query);
    std::string encodedLang = UrlEncodeUtf8(Utf8ToUtf16(lang.empty() ? "eng" : lang));
    // Replace {query} and {lang} / {languages}
    auto ReplaceAll = [](std::string& str, const std::string& from, const std::string& to) {
        if (from.empty()) return;
        size_t start_pos = 0;
        while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
            str.replace(start_pos, from.length(), to);
            start_pos += to.length();
        }
    };
    ReplaceAll(templ, "{query}", encodedQuery);
    ReplaceAll(templ, "{q}", encodedQuery);
    ReplaceAll(templ, "{lang}", encodedLang);
    ReplaceAll(templ, "{languages}", encodedLang);
    ReplaceAll(templ, "{language}", encodedLang);
    return Utf8ToUtf16(templ);
}

std::vector<OnlineSubtitleSearchResult> OnlineSubtitleService::ParseSearchResultsJson( const std::string& jsonText,
    const std::wstring &preferredLang) {
    std::vector<OnlineSubtitleSearchResult> results;
    if (jsonText.empty()) return results;
    JsonValue root;
    if (!JsonValue::parse(jsonText, root)) { return results; }
    const std::vector<JsonValue>* itemsArray = nullptr;
    if (root.is_array()) { itemsArray = &root.as_array(); } else if (root.is_object()) {
        if (root.has_key("data") && root.get("data").is_array()) {
            itemsArray = &root.get("data").as_array();
        } else if (root.has_key("results") && root.get("results").is_array()) {
            itemsArray = &root.get("results").as_array();
        } else if (root.has_key("subtitles") && root.get("subtitles").is_array()) {
            itemsArray = &root.get("subtitles").as_array();
        } else if (root.has_key("items") && root.get("items").is_array()) { itemsArray = &root.get("items").as_array(); }
    }
    if (!itemsArray) return results;
    for (const auto& item : *itemsArray) {
        if (!item.is_object()) continue;
        if (results.size() >= kMaxSearchResults) break;
        OnlineSubtitleSearchResult entry;
        // Try extracting attributes if using OpenSubtitles REST API v1 shape
        const JsonValue* targetObj = &item;
        if (item.has_key("attributes") && item.get("attributes").is_object()) { targetObj = &item.get("attributes"); }
        // ID
        if (item.has_key("id")) {
            entry.id = item.get("id").as_string();
            if (entry.id.empty() && item.get("id").is_number()) { entry.id = std::to_string(item.get("id").as_int()); }
        } else if (targetObj->has_key("subtitle_id")) {
            entry.id = targetObj->get("subtitle_id").as_string();
        } else if (targetObj->has_key("file_id")) { entry.id = targetObj->get("file_id").as_string(); }
        // Title / Name / Release
        if (targetObj->has_key("title") && targetObj->get("title").is_string()) {
            entry.title = Utf8ToUtf16(targetObj->get("title").as_string());
        } else if (targetObj->has_key("name") && targetObj->get("name").is_string()) {
            entry.title = Utf8ToUtf16(targetObj->get("name").as_string());
        } else if (targetObj->has_key("release") && targetObj->get("release").is_string()) {
            entry.title = Utf8ToUtf16(targetObj->get("release").as_string());
        } else if (targetObj->has_key("filename") && targetObj->get("filename").is_string()) {
            entry.title = Utf8ToUtf16(targetObj->get("filename").as_string());
        }
        // Language
        if (targetObj->has_key("language") && targetObj->get("language").is_string()) {
            entry.language = Utf8ToUtf16(targetObj->get("language").as_string());
        } else if (targetObj->has_key("lang") && targetObj->get("lang").is_string()) {
            entry.language = Utf8ToUtf16(targetObj->get("lang").as_string());
        } else if (targetObj->has_key("language_name") && targetObj->get("language_name").is_string()) {
            entry.language = Utf8ToUtf16(targetObj->get("language_name").as_string());
        } else { entry.language = preferredLang.empty() ? L"English" : preferredLang; }
        // Download URL (MUST be https://)
        if (targetObj->has_key("url") && targetObj->get("url").is_string()) {
            entry.downloadUrl = targetObj->get("url").as_string();
        } else if (targetObj->has_key("download_url") && targetObj->get("download_url").is_string()) {
            entry.downloadUrl = targetObj->get("download_url").as_string();
        } else if (targetObj->has_key("link") && targetObj->get("link").is_string()) {
            entry.downloadUrl = targetObj->get("link").as_string();
        } else if (targetObj->has_key("files") && targetObj->get("files").is_array()) {
            const auto& files = targetObj->get("files").as_array();
            if (!files.empty() && files[0].is_object()) {
                if (files[0].has_key("url") && files[0].get("url").is_string()) {
                    entry.downloadUrl = files[0].get("url").as_string();
                } else if (files[0].has_key("file_id")) {
                    entry.downloadUrl = "https://api.opensubtitles.com/api/v1/download/" + files[0].get("file_id").as_string();
                }
            }
        }
        // Verify URL starts with https://
        if (entry.downloadUrl.rfind("https://", 0) != 0) {
            continue; // Discard non-HTTPS or invalid URLs
        }
        // Format / Extension
        if (targetObj->has_key("format") && targetObj->get("format").is_string()) {
            entry.format = targetObj->get("format").as_string();
        } else if (targetObj->has_key("extension") && targetObj->get("extension").is_string()) {
            entry.format = targetObj->get("extension").as_string();
        } else { entry.format = "srt"; }
        std::transform(entry.format.begin(), entry.format.end(), entry.format.begin(), ::tolower);
        if (entry.format.empty() || (entry.format != "srt" && entry.format != "vtt" && entry.format != "ass" && entry.format != "ssa"))
            entry.format = "srt";
        // Score / Rating / Downloads
        if (targetObj->has_key("score")) { entry.score = targetObj->get("score").as_int(); } else if (targetObj->has_key("rating")) {
            entry.score = static_cast<int>(targetObj->get("rating").as_number() * 10.0);
        } else if (targetObj->has_key("download_count")) { entry.score = targetObj->get("download_count").as_int(); }
        if (entry.title.empty()) { entry.title = L"Subtitle " + Utf8ToUtf16(entry.id); }
        results.push_back(std::move(entry));
    }
    return results;
}

bool OnlineSubtitleService::ValidateSubtitleContent( const std::string& rawBytes, std::string& outNormalizedText,
    SubtitleEncoding &outEncoding) {
    if (rawBytes.empty()) return false;
    if (rawBytes.size() > SubtitleDiscovery::kMaxSubtitleInputBytes) return false;
    // Detect encoding & decode to normalized UTF-8 string
    outEncoding = SubtitleDiscovery::DetectEncoding(rawBytes);
    outNormalizedText = SubtitleDiscovery::DecodeSubtitleBytes(rawBytes, &outEncoding);
    if (outNormalizedText.empty()) return false;
    // Verify subtitle cues by attempting parsing against SRT, VTT, or ASS
    std::vector<SubtitleCue> cues;
    bool parsed = false;
    if (SubtitleEngine::ParseSrt(outNormalizedText, cues) && !cues.empty()) { parsed = true; } else {
        cues.clear();
        if (SubtitleEngine::ParseVtt(outNormalizedText, cues) && !cues.empty()) { parsed = true; } else {
            cues.clear();
            if (SubtitleEngine::ParseAss(outNormalizedText, cues) && !cues.empty()) { parsed = true; }
        }
    }
    if (!parsed || cues.empty()) { return false; }
    // Verify cue sanity: bounds and timestamps
    if (cues.size() > SubtitleEngine::kMaxSubtitleCueCount) { return false; }
    for (const auto& cue : cues) {
        if (cue.startTimeSec < 0.0 || cue.endTimeSec < cue.startTimeSec) { return false; }
        if (cue.text.size() * sizeof(wchar_t) > SubtitleEngine::kMaxCueTextBytes) { return false; }
    }
    return true;
}

StorageStatus OnlineSubtitleService::SaveSubtitleAtomically( const std::string& validatedContent, const std::wstring& queryTitle,
    const std::wstring& langCode, const std::string& idOrHash, const std::string& formatExt, std::wstring& outSavedPath,
    const std::wstring& rootDir)
{
    outSavedPath.clear();
    if (validatedContent.empty())
        return StorageStatus::Error(StorageErrorCode::EmptyData, ERROR_INVALID_PARAMETER, L"", "Validated subtitle data is empty");
    // Root-local cache directory: <rootDir>\cache\subtitles\online
    std::wstring subCacheBase = GetCacheDir(CacheClass::Subtitles, rootDir);
    std::wstring onlineSubDir = subCacheBase + L"\\online";
    DWORD dirAttr = GetFileAttributesW(onlineSubDir.c_str());
    if (dirAttr == INVALID_FILE_ATTRIBUTES || !(dirAttr & FILE_ATTRIBUTE_DIRECTORY)) {
        int res = SHCreateDirectoryExW(NULL, onlineSubDir.c_str(), NULL);
        if (res != ERROR_SUCCESS && res != ERROR_ALREADY_EXISTS && res != ERROR_FILE_EXISTS) {
            DWORD err = GetLastError();
            return StorageStatus::Error(StorageErrorCode::DirectoryCreationFailed, err ? err : (DWORD)res, onlineSubDir, "Failed to create online subtitles cache directory");
        }
    }
    // Build safe filename
    std::wstring safeTitle = queryTitle;
    for (wchar_t& c : safeTitle) {
        if (!::iswalnum(c) && c != L'_' && c != L'-') { c = L'_'; }
    }
    if (safeTitle.empty()) safeTitle = L"subtitle";
    if (safeTitle.length() > 64) safeTitle = safeTitle.substr(0, 64);
    std::wstring safeLang = langCode.empty() ? L"eng" : langCode;
    for (wchar_t& c : safeLang) { if (!::iswalnum(c)) c = L'_'; }
    std::wstring safeId = Utf8ToUtf16(idOrHash);
    for (wchar_t& c : safeId) { if (!::iswalnum(c)) c = L'_'; }
    if (safeId.empty()) { uint64_t hash = Fnv1a64(validatedContent.data(), validatedContent.size()); safeId = std::to_wstring(hash); }
    std::wstring ext = Utf8ToUtf16(formatExt);
    if (ext.empty() || ext[0] != L'.') ext = L"." + ext;
    if (ext != L".srt" && ext != L".vtt" && ext != L".ass" && ext != L".ssa") { ext = L".srt"; }
    std::wstring fileName = safeTitle + L"." + safeLang + L"." + safeId + ext;
    if (SubtitleDiscovery::IsReservedDeviceName(fileName)) { fileName = L"sub_" + safeId + ext; }
    std::wstring fullTargetPath = onlineSubDir + L"\\" + fileName;
    // Verify target path containment within root cache
    if (!SubtitleDiscovery::IsPathContained(subCacheBase, fullTargetPath))
        return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, fullTargetPath, "Target subtitle path escapes root cache directory");
    // Atomic Write Sequence with immediate temporary file cleanup on failure
    std::wstring tmpPath = fullTargetPath + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetCurrentThreadId()) + L"." + std::to_wstring(GetTickCount64());
    HANDLE hFile = CreateFileW( tmpPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return StorageStatus::Error(StorageErrorCode::FileOpenFailed, err, tmpPath, "Failed to create temporary file for atomic subtitle storage");
    }
    DWORD written = 0;
    BOOL writeOk = WriteFile(hFile, validatedContent.data(), static_cast<DWORD>(validatedContent.size()), &written, NULL);
    writeOk = writeOk && (written == static_cast<DWORD>(validatedContent.size()));
    DWORD writeErr = writeOk ? 0 : GetLastError();
    if (writeOk) { FlushFileBuffers(hFile); }
    CloseHandle(hFile);
    if (!writeOk) {
        DeleteFileW(tmpPath.c_str());
        return StorageStatus::Error(StorageErrorCode::FileWriteFailed, writeErr, tmpPath, "Failed to write data to temporary subtitle file");
    }
    if (!MoveFileExW(tmpPath.c_str(), fullTargetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD moveErr = GetLastError();
        DeleteFileW(tmpPath.c_str());
        return StorageStatus::Error(StorageErrorCode::AtomicRenameFailed, moveErr, fullTargetPath, "Failed to atomically rename temporary subtitle file");
    }
    outSavedPath = fullTargetPath;
    return StorageStatus::Ok(fullTargetPath);
}

OnlineSubtitleResult OnlineSubtitleService::Search( const std::wstring& mediaPath, const std::wstring& overrideTitle,
    const Settings &settings, IHttpTransport *customTransport, std::shared_ptr<OperationCancellation> cancellation) {
    OnlineSubtitleResult result;
    const auto cancelled = [&] { if (!cancellation || !cancellation->IsCancelled()) return false;
        result.status = OnlineSubtitleStatus::Cancelled; result.errorMessage = L"Subtitle search cancelled"; return true; };
    if (cancelled()) return result;
    if (!settings.onlineSubtitlesEnabled) {
        result.status = OnlineSubtitleStatus::Disabled;
        result.errorMessage = L"Online Subtitle Retrieval is disabled by user settings";
        return result;
    }
    std::wstring query = DeriveSearchQuery(mediaPath, overrideTitle);
    if (query.empty()) {
        result.status = OnlineSubtitleStatus::EmptyQuery;
        result.errorMessage = L"Could not derive search query from media title";
        return result;
    }
    if (!IsValidEndpointTemplate(settings.onlineSubtitleEndpoint)) {
        result.status = OnlineSubtitleStatus::InvalidEndpoint;
        result.errorMessage = L"Configured subtitle endpoint is invalid or does not use HTTPS";
        return result;
    }
    std::wstring searchUrl = BuildSearchUrl(settings.onlineSubtitleEndpoint, query, settings.subLang);
    IHttpTransport* transport = customTransport ? customTransport : defaultTransport_.get();
    HttpRequestOptions options;
    options.maxResponseBytes = 1024 * 1024; // 1 MB search response limit
    options.timeoutMs = 5000;
    options.maxRedirects = 3;
    options.cancellation = cancellation;
    HttpResponse response = transport->Get(searchUrl, options);
    result.httpStatusCode = response.statusCode;
    if (cancelled()) return result;
    if (!response.success) {
        result.status = MapHttpTransportStatus(response.status);
        result.errorMessage = response.errorMessage.empty() ? L"Network request failed" : response.errorMessage;
        return result;
    }
    result.searchResults = ParseSearchResultsJson(response.body, Utf8ToUtf16(settings.subLang));
    if (result.searchResults.empty()) {
        result.status = OnlineSubtitleStatus::NoResultsFound;
        result.errorMessage = L"No online subtitles found for '" + query + L"'";
        return result;
    }
    result.success = true;
    result.status = OnlineSubtitleStatus::Success;
    return result;
}

OnlineSubtitleResult OnlineSubtitleService::DownloadAndLoad( const OnlineSubtitleSearchResult& item, const std::wstring& mediaPath,
    const Settings& settings, IHttpTransport* customTransport, const std::wstring& rootDir,
    std::shared_ptr<OperationCancellation> cancellation)
{
    OnlineSubtitleResult result;
    const auto cancelled = [&] { if (!cancellation || !cancellation->IsCancelled()) return false;
        result.status = OnlineSubtitleStatus::Cancelled; result.errorMessage = L"Subtitle download cancelled"; return true; };
    if (cancelled()) return result;
    if (!settings.onlineSubtitlesEnabled) {
        result.status = OnlineSubtitleStatus::Disabled;
        result.errorMessage = L"Online Subtitle Retrieval is disabled by user settings";
        return result;
    }
    if (item.downloadUrl.rfind("https://", 0) != 0) {
        result.status = OnlineSubtitleStatus::HttpsRequired;
        result.errorMessage = L"Subtitle download URL must use HTTPS";
        return result;
    }
    IHttpTransport* transport = customTransport ? customTransport : defaultTransport_.get();
    HttpRequestOptions options;
    options.maxResponseBytes = kMaxSubtitleFileBytes; // 4 MB limit
    options.timeoutMs = 10000;                       // 10 seconds for file download
    options.maxRedirects = 3;
    options.cancellation = cancellation;
    HttpResponse response = transport->Get(Utf8ToUtf16(item.downloadUrl), options);
    result.httpStatusCode = response.statusCode;
    if (cancelled()) return result;
    if (!response.success) {
        result.status = MapHttpTransportStatus(response.status);
        result.errorMessage = response.errorMessage.empty() ? L"Failed to download subtitle file" : response.errorMessage;
        return result;
    }
    // Validate Content
    std::string normalizedText;
    SubtitleEncoding encoding = SubtitleEncoding::Unknown;
    if (!ValidateSubtitleContent(response.body, normalizedText, encoding)) {
        result.status = OnlineSubtitleStatus::InvalidSubtitleContent;
        result.errorMessage = L"Downloaded file is not a valid subtitle format or contains no cues";
        return result;
    }
    // Save Atomically to Root-Local Cache
    if (cancelled()) return result;
    std::wstring queryTitle = DeriveSearchQuery(mediaPath, item.title);
    std::wstring savedPath;
    StorageStatus saveStatus = SaveSubtitleAtomically( normalizedText, queryTitle, item.language, item.id, item.format, savedPath, rootDir);
    if (!saveStatus.is_ok()) {
        result.status = OnlineSubtitleStatus::StorageError;
        result.errorMessage = Utf8ToUtf16(saveStatus.message);
        return result;
    }
    if (cancelled()) return result;
    result.success = true;
    result.status = OnlineSubtitleStatus::Success;
    result.savedFilePath = savedPath;
    return result;
}

} // namespace Aperture
