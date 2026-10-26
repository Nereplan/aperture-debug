#include "NetworkStream.h"
#include "Common.h"
#include "Json.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <chrono>

namespace Aperture {

static std::wstring ToLower(const std::wstring& s) {
    std::wstring result = s;
    for (auto& c : result) {
        if (c >= L'A' && c <= L'Z') { c = c + (L'a' - L'A'); }
    }
    return result;
}

std::wstring NetworkStreamUrl::GetDisplayUrl(bool redactCredentials) const {
    if (!isValid && rawUrl.empty()) return L"";
    if (!redactCredentials) return rawUrl;
    std::wstring result;
    result.reserve(rawUrl.size());
    result.append(schemeStr).append(L"://");
    if (hasCredentials) result.append(username).append(username.empty() ? L"***@" : L":***@");
    result.append(host);
    if (port > 0) result.append(L":").append(std::to_wstring(port));
    result.append(path);
    static constexpr std::wstring_view sensitiveKeys[] = {
        L"token", L"key", L"secret", L"auth", L"password", L"pwd", L"pass", L"api_key", L"signature", L"sig"
    };
    // Scan once without lowercasing or rewriting the entire query per key.
    // '?' also starts a key in nested, unescaped URLs; redact its value through
    // the next '&', matching the existing credential-redaction boundary.
    for (size_t pos = 0; pos < query.size();) {
        if (query[pos] == L'?' || query[pos] == L'&') result += query[pos++];
        const size_t end = query.find_first_of(L"?&", pos);
        const size_t boundary = end == std::wstring::npos ? query.size() : end;
        const size_t equals = query.find(L'=', pos);
        bool sensitive = false;
        if (equals < boundary) {
            const std::wstring_view key(query.data() + pos, equals - pos);
            for (const auto candidate : sensitiveKeys) {
                if (key.size() == candidate.size() && std::equal(key.begin(), key.end(), candidate.begin(), [](wchar_t a, wchar_t b) {
                    return (a >= L'A' && a <= L'Z' ? a + (L'a' - L'A') : a) == b;
                })) { sensitive = true; break; }
            }
        }
        if (sensitive) {
            result.append(query, pos, equals - pos + 1).append(L"***");
            const size_t next = query.find(L'&', equals + 1);
            pos = next == std::wstring::npos ? query.size() : next;
        } else {
            result.append(query, pos, boundary - pos);
            pos = boundary;
        }
    }
    return result.append(fragment);
}

bool NetworkStreamValidator::ContainsControlCharacters(const std::wstring& text) {
    for (wchar_t c : text) {
        if (c < 32 || c == 127) { return true; }
    }
    return false;
}

bool NetworkStreamValidator::IsAllowedScheme(const std::wstring& scheme, const NetworkStreamValidationOptions& options) {
    std::wstring lower = ToLower(scheme);
    if (lower == L"http") return options.allowHttp;
    if (lower == L"https") return options.allowHttps;
    if (lower == L"rtsp") return options.allowRtsp;
    if (lower == L"rtmp") return options.allowRtmp;
    if (lower == L"udp") return options.allowUdp;
    if (lower == L"rtp") return options.allowRtp;
    return false;
}

bool NetworkStreamValidator::IsValidIPv4(const std::wstring& host) {
    int parts[4] = { 0 };
    int partIdx = 0, currentNum = 0;
    bool hasDigits = false;
    for (size_t i = 0; i < host.size(); ++i) {
        wchar_t c = host[i];
        if (c >= L'0' && c <= L'9') {
            currentNum = currentNum * 10 + (c - L'0');
            if (currentNum > 255) return false;
            hasDigits = true;
        } else if (c == L'.') {
            if (!hasDigits || partIdx >= 3) return false;
            parts[partIdx++] = currentNum;
            currentNum = 0;
            hasDigits = false;
        } else { return false; }
    }
    if (!hasDigits || partIdx != 3) return false;
    parts[partIdx] = currentNum;
    return true;
}

bool NetworkStreamValidator::IsValidIPv6(const std::wstring& host) {
    // Expected format: [ipv6_address] or [ipv6%scope]
    if (host.size() < 4) return false;
    if (host.front() != L'[' || host.back() != L']') return false;
    std::wstring inner = host.substr(1, host.size() - 2);
    // Handle optional scope ID e.g. %eth0 or %1
    size_t scopePos = inner.find(L'%');
    if (scopePos != std::wstring::npos) { inner = inner.substr(0, scopePos); }
    if (inner.empty()) return false;
    if (inner.find(L":::") != std::wstring::npos) return false;
    int doubleColonCount = 0, colons = 0;
    size_t len = inner.size();
    // Check for :: (double colon)
    for (size_t i = 0; i < len; ++i) {
        if (inner[i] == L':') {
            colons++;
            if (i + 1 < len && inner[i + 1] == L':') {
                doubleColonCount++;
                i++; // Skip the second colon
            }
        }
    }
    if (doubleColonCount > 1) return false;
    // Check all characters are valid hex, colons, or dots (for IPv4-mapped)
    for (wchar_t c : inner) {
        bool isHex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
        if (!isHex && c != L':' && c != L'.') { return false; }
    }
    // Split by colons and validate each group
    std::vector<std::wstring> groups;
    size_t start = 0;
    while (start <= inner.size()) {
        size_t end = inner.find(L':', start);
        if (end == std::wstring::npos) end = inner.size();
        groups.push_back(inner.substr(start, end - start));
        start = end + 1;
    }
    if (doubleColonCount == 0 && groups.size() != 8) {
        // Must have exactly 8 groups if no double colon
        return false;
    }
    if (doubleColonCount > 0 && groups.size() > 8) { return false; }
    for (const auto& g : groups) {
        if (g.empty()) continue; // Valid for ::
        if (g.find(L'.') != std::wstring::npos) {
            // IPv4-mapped trailing part e.g. ::ffff:192.168.1.1
            if (!IsValidIPv4(g)) return false;
        } else {
            if (g.size() > 4) return false;
            for (wchar_t c : g) {
                bool isHex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
                if (!isHex) return false;
            }
        }
    }
    return true;
}

bool NetworkStreamValidator::IsValidDomain(const std::wstring& host) {
    if (host.empty() || host.size() > 253) return false;
    size_t start = 0, dotCount = 0;
    while (start < host.size()) {
        size_t end = host.find(L'.', start);
        if (end == std::wstring::npos) end = host.size();
        std::wstring label = host.substr(start, end - start);
        if (label.empty() || label.size() > 63) return false;
        // Label must not start or end with hyphen
        if (label.front() == L'-' || label.back() == L'-') return false;
        for (wchar_t c : label) {
            bool isAlnum = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
            if (!isAlnum && c != L'-') { return false; }
        }
        if (end < host.size()) dotCount++;
        start = end + 1;
    }
    return true;
}

bool NetworkStreamValidator::IsValidHost(const std::wstring& host) {
    if (host.empty()) return false;
    if (host.front() == L'[' || host.back() == L']') return IsValidIPv6(host);
    bool numericIPv4Like = true;
    for (wchar_t c : host) {
        if ((c < L'0' || c > L'9') && c != L'.') { numericIPv4Like = false; break; }
    }
    if (numericIPv4Like) return IsValidIPv4(host);
    return IsValidDomain(host);
}

NetworkStreamUrl NetworkStreamValidator::ValidateAndParse(const std::wstring &inputUrl, const NetworkStreamValidationOptions &options) {
    NetworkStreamUrl result;
    size_t trimStart = 0;
    size_t trimEnd = inputUrl.size();
    while (trimStart < trimEnd && (inputUrl[trimStart] == L' ' || inputUrl[trimStart] == L'\t' || inputUrl[trimStart] == L'\r' || inputUrl[trimStart] == L'\n')) ++trimStart;
    while (trimEnd > trimStart && (inputUrl[trimEnd - 1] == L' ' || inputUrl[trimEnd - 1] == L'\t' || inputUrl[trimEnd - 1] == L'\r' || inputUrl[trimEnd - 1] == L'\n')) --trimEnd;
    const std::wstring rawUrl = inputUrl.substr(trimStart, trimEnd - trimStart);
    result.rawUrl = rawUrl;
    result.isValid = false;
    if (rawUrl.empty()) { result.error = NetworkStreamValidationError::EmptyUrl; return result; }
    if (rawUrl.length() > options.maxLength) { result.error = NetworkStreamValidationError::UrlTooLong; return result; }
    if (ContainsControlCharacters(rawUrl)) { result.error = NetworkStreamValidationError::ContainsControlCharacters; return result; }
    // Find scheme delimiter "://"
    size_t schemeDelim = rawUrl.find(L"://");
    if (schemeDelim == std::wstring::npos) { result.error = NetworkStreamValidationError::MissingSchemeDelimiter; return result; }
    std::wstring schemePart = rawUrl.substr(0, schemeDelim);
    if (schemePart.empty()) { result.error = NetworkStreamValidationError::UnsupportedScheme; return result; }
    // Check scheme validity
    std::wstring lowerScheme = ToLower(schemePart);
    if (!IsAllowedScheme(lowerScheme, options)) { result.error = NetworkStreamValidationError::UnsupportedScheme; return result; }
    result.schemeStr = lowerScheme;
    if (lowerScheme == L"http") result.scheme = NetworkStreamScheme::Http;
    else if (lowerScheme == L"https") result.scheme = NetworkStreamScheme::Https;
    else if (lowerScheme == L"rtsp") result.scheme = NetworkStreamScheme::Rtsp;
    else if (lowerScheme == L"rtmp") result.scheme = NetworkStreamScheme::Rtmp;
    else if (lowerScheme == L"udp") result.scheme = NetworkStreamScheme::Udp;
    else if (lowerScheme == L"rtp") result.scheme = NetworkStreamScheme::Rtp;
    else result.scheme = NetworkStreamScheme::Unknown;
    // Parse authority and path/query/fragment
    size_t authorityStart = schemeDelim + 3;
    size_t pathStart = rawUrl.find_first_of(L"/?#", authorityStart);
    std::wstring authority;
    if (pathStart == std::wstring::npos) { authority = rawUrl.substr(authorityStart); result.path = L"/"; } else {
        authority = rawUrl.substr(authorityStart, pathStart - authorityStart);
        // Find query and fragment
        size_t queryStart = rawUrl.find(L'?', pathStart);
        size_t fragStart = rawUrl.find(L'#', pathStart);
        if (queryStart != std::wstring::npos && (fragStart == std::wstring::npos || queryStart < fragStart)) {
            result.path = rawUrl.substr(pathStart, queryStart - pathStart);
            if (fragStart != std::wstring::npos) {
                result.query = rawUrl.substr(queryStart, fragStart - queryStart);
                result.fragment = rawUrl.substr(fragStart);
            } else { result.query = rawUrl.substr(queryStart); }
        } else if (fragStart != std::wstring::npos) {
            result.path = rawUrl.substr(pathStart, fragStart - pathStart);
            result.fragment = rawUrl.substr(fragStart);
        } else { result.path = rawUrl.substr(pathStart); }
    }
    if (authority.empty()) { result.error = NetworkStreamValidationError::EmptyHost; return result; }
    // Parse credentials from authority
    std::wstring hostPort = authority;
    size_t atPos = authority.find(L'@');
    if (atPos != std::wstring::npos) {
        result.hasCredentials = true;
        std::wstring userinfo = authority.substr(0, atPos);
        hostPort = authority.substr(atPos + 1);
        size_t colonPos = userinfo.find(L':');
        if (colonPos != std::wstring::npos) {
            result.username = userinfo.substr(0, colonPos);
            result.password = userinfo.substr(colonPos + 1);
        } else { result.username = userinfo; }
        if (!options.allowCredentials) { result.error = NetworkStreamValidationError::EmbeddedCredentialsNotAllowed; return result; }
    }
    // Parse Host and Port
    if (hostPort.empty()) { result.error = NetworkStreamValidationError::EmptyHost; return result; }
    std::wstring hostPart, portPart;
    if (hostPort.front() == L'[') {
        // IPv6 host
        size_t bracketEnd = hostPort.find(L']');
        if (bracketEnd == std::wstring::npos) { result.error = NetworkStreamValidationError::InvalidHost; return result; }
        hostPart = hostPort.substr(0, bracketEnd + 1);
        if (bracketEnd + 1 < hostPort.size()) {
            if (hostPort[bracketEnd + 1] != L':') { result.error = NetworkStreamValidationError::InvalidHost; return result; }
            portPart = hostPort.substr(bracketEnd + 2);
        }
    } else {
        size_t colonPos = hostPort.find(L':');
        if (colonPos != std::wstring::npos) {
            hostPart = hostPort.substr(0, colonPos);
            portPart = hostPort.substr(colonPos + 1);
        } else { hostPart = hostPort; }
    }
    if (hostPart.empty()) { result.error = NetworkStreamValidationError::EmptyHost; return result; }
    if (!IsValidHost(hostPart)) { result.error = NetworkStreamValidationError::InvalidHost; return result; }
    result.host = hostPart;
    // Validate port if present
    if (!portPart.empty()) {
        for (wchar_t c : portPart) {
            if (c < L'0' || c > L'9') { result.error = NetworkStreamValidationError::InvalidPort; return result; }
        }
        try {
            int p = std::stoi(portPart);
            if (p <= 0 || p > 65535) { result.error = NetworkStreamValidationError::InvalidPort; return result; }
            result.port = p;
        } catch (...) { result.error = NetworkStreamValidationError::InvalidPort; return result; }
    }
    result.isValid = true;
    result.error = NetworkStreamValidationError::None;
    return result;
}

std::wstring NetworkStreamValidator::RedactUrl(const std::wstring& rawUrl) {
    if (rawUrl.empty()) return L"";
    // Attempt to validate and parse with credentials allowed so we can safely redact
    NetworkStreamValidationOptions opts;
    opts.allowCredentials = true;
    NetworkStreamUrl parsed = ValidateAndParse(rawUrl, opts);
    if (parsed.isValid) { return parsed.GetRedactedUrl(); }
    // Fallback safe regex-free string redaction for malformed strings
    std::wstring result = rawUrl;
    size_t schemePos = result.find(L"://");
    if (schemePos != std::wstring::npos) {
        size_t authStart = schemePos + 3;
        size_t slashPos = result.find_first_of(L"/?#", authStart);
        size_t atPos = result.find(L'@', authStart);
        if (atPos != std::wstring::npos && (slashPos == std::wstring::npos || atPos < slashPos)) {
            // Replace user:pass with ***
            result.replace(authStart, atPos - authStart, L"***");
        }
    }
    return result;
}

std::wstring NetworkStreamValidator::GetSafeErrorMessage(NetworkStreamValidationError error, const std::wstring& rawUrl) {
    std::wstring safeUrl = RedactUrl(rawUrl);
    switch (error) {
    case NetworkStreamValidationError::None: return L"";
    case NetworkStreamValidationError::EmptyUrl: return L"Stream URL cannot be empty.";
    case NetworkStreamValidationError::UrlTooLong: return L"Stream URL exceeds maximum allowed length.";
    case NetworkStreamValidationError::ContainsControlCharacters: return L"Stream URL contains invalid control characters.";
    case NetworkStreamValidationError::UnsupportedScheme:
        return L"Unsupported stream protocol. Only http, https, rtsp, rtmp, udp, and rtp are supported.";
    case NetworkStreamValidationError::MissingSchemeDelimiter: return L"Invalid URL format. Protocol delimiter '://' is required.";
    case NetworkStreamValidationError::InvalidHost: return L"Invalid stream host or IP address.";
    case NetworkStreamValidationError::EmptyHost: return L"Stream URL must specify a host.";
    case NetworkStreamValidationError::InvalidPort: return L"Invalid port number. Port must be between 1 and 65535.";
    case NetworkStreamValidationError::EmbeddedCredentialsNotAllowed:
        return L"Embedded credentials in stream URLs are not permitted for security.";
    case NetworkStreamValidationError::InvalidCharacters: return L"Stream URL contains illegal characters.";
    default: return L"Invalid stream URL.";
    }
}

std::wstring NetworkStreamValidator::ExtractCandidateUrl(const std::wstring& rawText) {
    if (rawText.empty()) return L"";
    const size_t maxScan = 4096;
    std::wstring bounded = rawText.substr(0, maxScan);
    size_t start = 0;
    while (start < bounded.size()) {
        size_t end = bounded.find_first_of(L"\r\n", start);
        if (end == std::wstring::npos) end = bounded.size();
        std::wstring line = bounded.substr(start, end - start);
        size_t first = line.find_first_not_of(L" \t\r\n\v\f");
        if (first != std::wstring::npos) {
            size_t last = line.find_last_not_of(L" \t\r\n\v\f");
            std::wstring trimmed = line.substr(first, (last - first + 1));
            if (!trimmed.empty() && trimmed.front() != L'#') { return trimmed; }
        }
        start = end + 1;
        while (start < bounded.size() && (bounded[start] == L'\r' || bounded[start] == L'\n')) { start++; }
    }
    return L"";
}

DroppedUrlResult NetworkStreamValidator::ExtractAndValidateDroppedText( const std::wstring& rawText,
    const NetworkStreamValidationOptions &options) {
    DroppedUrlResult result;
    std::wstring candidate = ExtractCandidateUrl(rawText);
    if (candidate.empty()) { result.hasUrl = false; return result; }
    result.hasUrl = true;
    result.extractedUrl = candidate;
    result.parsedUrl = ValidateAndParse(candidate, options);
    if (!result.parsedUrl.isValid) { result.safeErrorMessage = GetSafeErrorMessage(result.parsedUrl.error, candidate); }
    return result;
}

namespace {

static bool TryExtractWideHGlobal(IDataObject* pDataObj, CLIPFORMAT format, std::wstring& outText, size_t maxChars = 4096) {
    if (!pDataObj) return false;
    FORMATETC fmt = { format, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stg{};
    if (FAILED(pDataObj->GetData(&fmt, &stg))) { return false; }
    bool success = false;
    if (stg.hGlobal) {
        SIZE_T byteSize = GlobalSize(stg.hGlobal);
        if (byteSize >= sizeof(wchar_t)) {
            const wchar_t* pData = static_cast<const wchar_t*>(GlobalLock(stg.hGlobal));
            if (pData) {
                size_t numChars = byteSize / sizeof(wchar_t);
                size_t actualLen = 0;
                while (actualLen < numChars && actualLen < maxChars && pData[actualLen] != L'\0') { actualLen++; }
                if (actualLen > 0) { outText.assign(pData, actualLen); success = true; }
                GlobalUnlock(stg.hGlobal);
            }
        }
    }
    ReleaseStgMedium(&stg);
    return success;
}

static bool TryExtractAnsiHGlobal(IDataObject* pDataObj, CLIPFORMAT format, std::wstring& outText, size_t maxChars = 4096) {
    if (!pDataObj) return false;
    FORMATETC fmt = { format, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stg{};
    if (FAILED(pDataObj->GetData(&fmt, &stg))) { return false; }
    bool success = false;
    if (stg.hGlobal) {
        SIZE_T byteSize = GlobalSize(stg.hGlobal);
        if (byteSize > 0) {
            const char* pData = static_cast<const char*>(GlobalLock(stg.hGlobal));
            if (pData) {
                size_t actualLen = 0;
                while (actualLen < byteSize && actualLen < maxChars && pData[actualLen] != '\0') { actualLen++; }
                if (actualLen > 0) { std::string narrowStr(pData, actualLen); outText = Utf8ToUtf16(narrowStr); success = true; }
                GlobalUnlock(stg.hGlobal);
            }
        }
    }
    ReleaseStgMedium(&stg);
    return success;
}

} // namespace

DroppedUrlResult NetworkStreamValidator::ExtractAndValidateFromDataObject( IDataObject* pDataObj,
    const NetworkStreamValidationOptions &options) {
    DroppedUrlResult result;
    if (!pDataObj) return result;
    static const UINT cfInetUrlW = RegisterClipboardFormatW(L"UniformResourceLocatorW");
    static const UINT cfInetUrlA = RegisterClipboardFormatW(L"UniformResourceLocator");
    static const UINT cfUriList = RegisterClipboardFormatW(L"text/uri-list");
    static const UINT cfTextPlain = RegisterClipboardFormatW(L"text/plain");
    struct FormatSpec { CLIPFORMAT cf; bool isWide; };
    const FormatSpec formats[] = {
        { static_cast<CLIPFORMAT>(cfInetUrlW), true }, { static_cast<CLIPFORMAT>(cfInetUrlA), false },
        { static_cast<CLIPFORMAT>(cfUriList), false }, { CF_UNICODETEXT, true }, { static_cast<CLIPFORMAT>(cfTextPlain), false },
        { CF_TEXT, false }
    };
    for (const auto& f : formats) {
        if (f.cf == 0) continue;
        std::wstring rawText;
        bool extracted = f.isWide ? TryExtractWideHGlobal(pDataObj, f.cf, rawText) : TryExtractAnsiHGlobal(pDataObj, f.cf, rawText);
        if (extracted && !rawText.empty()) {
            DroppedUrlResult res = ExtractAndValidateDroppedText(rawText, options);
            if (res.hasUrl) { return res; }
        }
    }
    return result;
}

void NetworkStreamModel::AddRecentStream(const std::wstring& url, const std::wstring& title) {
    std::lock_guard<std::mutex> lock(modelMutex_);
    if (!config_.enableRecentStreams) return;
    // Validate URL before adding
    NetworkStreamUrl parsed = NetworkStreamValidator::ValidateAndParse(url, config_.validationOptions);
    if (!parsed.isValid) return;
    std::wstring safeUrl = parsed.GetRedactedUrl();
    // Deduplicate
    auto it = std::remove_if(recentStreams_.begin(), recentStreams_.end(),
                             [&safeUrl](const RecentStreamEntry &entry) { return entry.url == safeUrl; });
    recentStreams_.erase(it, recentStreams_.end());
    RecentStreamEntry entry;
    entry.url = safeUrl;
    entry.title = title.empty() ? safeUrl : title;
    entry.lastPlayedTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    recentStreams_.insert(recentStreams_.begin(), entry);
    if (recentStreams_.size() > config_.maxRecentStreams) { recentStreams_.resize(config_.maxRecentStreams); }
}

StorageStatus NetworkStreamModel::LoadRecentStreams(const std::wstring& baseDir, const std::wstring& fileName) {
    std::lock_guard<std::mutex> lock(modelMutex_);
    if (!config_.enableRecentStreams) { return StorageStatus::Ok(); }
    std::wstring fullPath = baseDir + L"\\" + fileName;
    std::string jsonContent;
    StorageStatus readStatus = ReadTextFile(fullPath, jsonContent);
    if (!readStatus.is_ok()) { return readStatus; }
    JsonValue root;
    if (!JsonValue::parse(jsonContent, root))
        return StorageStatus::Error(StorageErrorCode::SerializationFailed, 0, fullPath, "JSON parse error");
    if (!root.is_object()) { return StorageStatus::Error(StorageErrorCode::SerializationFailed, 0, fullPath, "Root must be an object"); }
    const JsonValue& arr = root.get("recentStreams");
    if (arr.is_array()) {
        recentStreams_.clear();
        const auto& values = arr.as_array();
        for (size_t i = 0; i < values.size() && i < config_.maxRecentStreams; ++i) {
            const JsonValue& item = values[i];
            if (!item.is_object()) continue;
            std::string urlUtf8 = item.get("url").as_string();
            std::string titleUtf8 = item.get("title").as_string();
            int64_t ts = item.get("lastPlayed").as_int64();
            std::wstring wideUrl = Utf8ToUtf16(urlUtf8);
            NetworkStreamUrl parsed = NetworkStreamValidator::ValidateAndParse(wideUrl, config_.validationOptions);
            if (parsed.isValid) {
                RecentStreamEntry entry;
                entry.url = parsed.GetRedactedUrl();
                entry.title = titleUtf8.empty() ? entry.url : Utf8ToUtf16(titleUtf8);
                entry.lastPlayedTimestamp = ts;
                recentStreams_.push_back(entry);
            }
        }
    }
    return StorageStatus::Ok(fullPath);
}

StorageStatus NetworkStreamModel::SaveRecentStreams(const std::wstring& baseDir, const std::wstring& fileName) {
    std::wstring fullPath;
    std::string jsonStr;
    {
        std::lock_guard<std::mutex> lock(modelMutex_);
        if (!config_.enableRecentStreams) { return StorageStatus::Ok(); }
        fullPath = baseDir + L"\\" + fileName;
        JsonValue root = JsonValue::Object();
        root["schemaVersion"] = JsonValue(static_cast<int64_t>(kCurrentStreamSchemaVersion));
        JsonValue arr = JsonValue::Array();
        for (const auto& item : recentStreams_) {
            JsonValue obj = JsonValue::Object();
            obj["url"] = JsonValue(Utf16ToUtf8(item.url));
            obj["title"] = JsonValue(Utf16ToUtf8(item.title));
            obj["lastPlayed"] = JsonValue(item.lastPlayedTimestamp);
            arr.push_back(obj);
        }
        root["recentStreams"] = arr;
        jsonStr = root.stringify(true);
    }
    return AtomicWriteTextFile(fullPath, jsonStr);
}

} // namespace Aperture
