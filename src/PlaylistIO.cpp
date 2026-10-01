#include "PlaylistIO.h"
#include "IpcProtocol.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cwctype>

namespace Aperture {

namespace {

// Helper: Trim string
std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

std::wstring TrimW(const std::wstring& str) {
    size_t first = str.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    size_t last = str.find_last_not_of(L" \t\r\n");
    return str.substr(first, (last - first + 1));
}

// Helper: Case-insensitive ascii string comparison
bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) { return false; }
    }
    return true;
}

bool StartsWithIgnoreCase(const std::string& str, const std::string& prefix) {
    if (str.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(str[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) { return false; }
    }
    return true;
}

bool StartsWithIgnoreCaseW(const std::wstring& str, const std::wstring& prefix) {
    if (str.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::towlower(str[i]) != std::towlower(prefix[i])) { return false; }
    }
    return true;
}

bool IsValidUtf8(const char* data, size_t size) {
    size_t i = 0;
    while (i < size) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        if (c <= 0x7F) { i += 1; } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= size) return false;
            if ((static_cast<unsigned char>(data[i + 1]) & 0xC0) != 0x80) return false;
            if (c < 0xC2) return false; // Overlong
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= size) return false;
            if ((static_cast<unsigned char>(data[i + 1]) & 0xC0) != 0x80) return false;
            if ((static_cast<unsigned char>(data[i + 2]) & 0xC0) != 0x80) return false;
            if (c == 0xE0 && static_cast<unsigned char>(data[i + 1]) < 0xA0) return false; // Overlong
            if (c == 0xED && static_cast<unsigned char>(data[i + 1]) >= 0xA0) return false; // UTF-16 surrogate
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= size) return false;
            if ((static_cast<unsigned char>(data[i + 1]) & 0xC0) != 0x80) return false;
            if ((static_cast<unsigned char>(data[i + 2]) & 0xC0) != 0x80) return false;
            if ((static_cast<unsigned char>(data[i + 3]) & 0xC0) != 0x80) return false;
            if (c == 0xF0 && static_cast<unsigned char>(data[i + 1]) < 0x90) return false; // Overlong
            if (c == 0xF4 && static_cast<unsigned char>(data[i + 1]) >= 0x90) return false; // > U+10FFFF
            i += 4;
        } else { return false; }
    }
    return true;
}

} // namespace

PlaylistFormat PlaylistIO::DetectFormatFromExtension(const std::wstring& path) {
    if (path.empty()) return PlaylistFormat::Auto;
    size_t dotPos = path.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return PlaylistFormat::Auto;
    std::wstring ext = path.substr(dotPos);
    for (auto& c : ext) c = std::towlower(c);
    if (ext == L".m3u8") return PlaylistFormat::M3U8;
    if (ext == L".m3u") return PlaylistFormat::M3U;
    if (ext == L".pls") return PlaylistFormat::PLS;
    return PlaylistFormat::Auto;
}

PlaylistFormat PlaylistIO::DetectFormatFromContent(const std::string& content) {
    std::string trimmed = Trim(content);
    if (StartsWithIgnoreCase(trimmed, "#EXTM3U") || StartsWithIgnoreCase(trimmed, "#EXTINF")) { return PlaylistFormat::M3U8; }
    if (StartsWithIgnoreCase(trimmed, "[playlist]")) { return PlaylistFormat::PLS; }
    // Check if contains [playlist] anywhere in first 1024 bytes
    std::string preview = trimmed.substr(0, std::min<size_t>(trimmed.size(), 1024));
    for (auto& c : preview) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (preview.find("[playlist]") != std::string::npos) { return PlaylistFormat::PLS; }
    if (preview.find("#extm3u") != std::string::npos || preview.find("#extinf") != std::string::npos) { return PlaylistFormat::M3U8; }
    return PlaylistFormat::M3U8;
}

bool PlaylistIO::IsUrl(const std::wstring& path) {
    if (path.empty()) return false;
    std::wstring trimmed = TrimW(path);
    if (StartsWithIgnoreCaseW(trimmed, L"http://") || StartsWithIgnoreCaseW(trimmed, L"https://") ||
        StartsWithIgnoreCaseW(trimmed, L"rtsp://") || StartsWithIgnoreCaseW(trimmed, L"rtmp://") ||
        StartsWithIgnoreCaseW(trimmed, L"mms://") || StartsWithIgnoreCaseW(trimmed, L"ftp://") ||
        StartsWithIgnoreCaseW(trimmed, L"ftps://") || StartsWithIgnoreCaseW(trimmed, L"hls://") ||
        StartsWithIgnoreCaseW(trimmed, L"udp://") || StartsWithIgnoreCaseW(trimmed, L"tcp://") ||
        StartsWithIgnoreCaseW(trimmed, L"smb://") || StartsWithIgnoreCaseW(trimmed, L"file://")) {
        return true;
    }
    // Check scheme pattern: alpha followed by alphanum/+/-/. then "://"
    size_t colonSlashSlash = trimmed.find(L"://");
    if (colonSlashSlash != std::wstring::npos && colonSlashSlash > 1 && colonSlashSlash < 16) {
        bool validScheme = true;
        if (!std::iswalpha(trimmed[0])) validScheme = false;
        for (size_t i = 1; i < colonSlashSlash && validScheme; ++i) {
            wchar_t ch = trimmed[i];
            if (!std::iswalnum(ch) && ch != L'+' && ch != L'-' && ch != L'.') { validScheme = false; }
        }
        if (validScheme) return true;
    }
    return false;
}

std::wstring PlaylistIO::NormalizeSeparators(const std::wstring& path) {
    std::wstring result = path;
    for (auto& c : result) { if (c == L'/') c = L'\\'; }
    return result;
}

std::wstring PlaylistIO::EnsureExtendedPath(const std::wstring& path) {
    if (path.empty()) return L"";
    if (IsUrl(path)) return path;
    std::wstring norm = NormalizeSeparators(path);
    if (norm.rfind(L"\\\\?\\", 0) == 0 || norm.rfind(L"\\\\.\\", 0) == 0) { return norm; }
    if (norm.length() >= MAX_PATH) {
        if (norm.rfind(L"\\\\", 0) == 0) {
            // UNC path: \\server\share -> \\?\UNC\server\share
            return L"\\\\?\\UNC\\" + norm.substr(2);
        } else if (norm.length() >= 2 && norm[1] == L':' && std::iswalpha(norm[0])) { return L"\\\\?\\" + norm; }
    }
    return norm;
}

std::wstring PlaylistIO::ResolveEntryLocation(const std::wstring &entryPath, const std::wstring &playlistDir, bool resolveRelative) {
    std::wstring trimmed = TrimW(entryPath);
    if (trimmed.empty()) return L"";
    if (IsUrl(trimmed)) return trimmed;
    std::wstring norm = NormalizeSeparators(trimmed);
    bool isAbsolute = false;
    if (norm.length() >= 2 && norm[1] == L':' && std::iswalpha(norm[0])) {
        isAbsolute = true;
    } else if (norm.rfind(L"\\\\", 0) == 0) {
        isAbsolute = true;
    } else if (norm.rfind(L"\\", 0) == 0) { isAbsolute = true; }
    if (isAbsolute) { return Ipc::ResolveFullPathLong(norm); }
    if (resolveRelative && !playlistDir.empty()) {
        std::wstring combined = playlistDir;
        if (!combined.empty() && combined.back() != L'\\' && combined.back() != L'/') { combined += L'\\'; }
        combined += norm;
        return Ipc::ResolveFullPathLong(combined);
    }
    return norm;
}

size_t PlaylistIO::AppendUniquePathsOrdinalIgnoreCase(std::vector<std::wstring>& existing,
    const std::vector<std::wstring>& candidates) {
    if (candidates.empty()) return 0;

    // Small drops need only a few linear passes; sorting a large existing
    // playlist would cost more than those passes.
    if (candidates.size() <= 8) {
        size_t added = 0;
        for (const auto& path : candidates) {
            const bool duplicate = std::any_of(existing.begin(), existing.end(), [&](const auto& item) {
                return CompareStringOrdinal(item.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
            if (!duplicate) { existing.push_back(path); ++added; }
        }
        return added;
    }

    // Sort index arrays instead of copying path strings. This changes the
    // membership work from one full scan per imported path to two sorts and
    // a merge while preserving CompareStringOrdinal's path equality semantics.
    std::vector<size_t> existingOrder(existing.size());
    for (size_t i = 0; i < existingOrder.size(); ++i) existingOrder[i] = i;
    std::vector<size_t> candidateOrder(candidates.size());
    for (size_t i = 0; i < candidateOrder.size(); ++i) candidateOrder[i] = i;

    auto compare = [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE);
    };
    std::sort(existingOrder.begin(), existingOrder.end(), [&](size_t a, size_t b) {
        return compare(existing[a], existing[b]) == CSTR_LESS_THAN;
    });
    std::sort(candidateOrder.begin(), candidateOrder.end(), [&](size_t a, size_t b) {
        return compare(candidates[a], candidates[b]) == CSTR_LESS_THAN;
    });

    auto skipEqualGroup = [&](const std::vector<size_t>& order, size_t at, const auto& paths) {
        size_t next = at + 1;
        while (next < order.size() && compare(paths[order[at]], paths[order[next]]) == CSTR_EQUAL) ++next;
        return next;
    };

    std::vector<size_t> additions;
    additions.reserve(candidates.size());
    size_t existingAt = 0;
    size_t candidateAt = 0;
    while (candidateAt < candidateOrder.size()) {
        const size_t candidateGroupEnd = skipEqualGroup(candidateOrder, candidateAt, candidates);
        // Advance existing groups within this candidate iteration so a large
        // candidate duplicate group is classified only once.
        while (existingAt < existingOrder.size() &&
               compare(existing[existingOrder[existingAt]], candidates[candidateOrder[candidateAt]]) == CSTR_LESS_THAN) {
            existingAt = skipEqualGroup(existingOrder, existingAt, existing);
        }
        if (existingAt == existingOrder.size() ||
            compare(existing[existingOrder[existingAt]], candidates[candidateOrder[candidateAt]]) == CSTR_GREATER_THAN) {
            size_t first = candidateOrder[candidateAt];
            for (size_t i = candidateAt + 1; i < candidateGroupEnd; ++i) first = std::min(first, candidateOrder[i]);
            additions.push_back(first);
        } else {
            existingAt = skipEqualGroup(existingOrder, existingAt, existing);
        }
        candidateAt = candidateGroupEnd;
    }

    // Keep first-seen import order for newly appended paths.
    std::sort(additions.begin(), additions.end());
    existing.reserve(existing.size() + additions.size());
    for (size_t index : additions) existing.push_back(candidates[index]);
    return additions.size();
}

std::wstring PlaylistIO::MakeRelativePathWhereSafe(const std::wstring &fullPath, const std::wstring &baseDir) {
    if (fullPath.empty()) return L"";
    if (IsUrl(fullPath)) return fullPath;
    if (baseDir.empty()) return fullPath;
    std::wstring normFull = NormalizeSeparators(fullPath);
    std::wstring normBase = NormalizeSeparators(baseDir);
    // Strip \\?\ prefix for comparison if present
    if (normFull.rfind(L"\\\\?\\", 0) == 0) normFull = normFull.substr(4);
    if (normBase.rfind(L"\\\\?\\", 0) == 0) normBase = normBase.substr(4);
    if (normFull.empty() || normBase.empty()) return fullPath;
    // Check drive letters
    if (normFull.length() >= 2 && normBase.length() >= 2 && normFull[1] == L':' && normBase[1] == L':') {
        if (std::towlower(normFull[0]) != std::towlower(normBase[0])) {
            return fullPath; // Cross-drive: cannot make relative
        }
    }
    wchar_t relBuffer[MAX_PATH * 2] = {0};
    if (PathRelativePathToW(relBuffer, normBase.c_str(), FILE_ATTRIBUTE_DIRECTORY, normFull.c_str(), FILE_ATTRIBUTE_NORMAL)) {
        std::wstring rel = relBuffer;
        if (rel.rfind(L".\\", 0) == 0) { rel = rel.substr(2); }
        // If relative path is not excessively backtracking and is valid
        if (!rel.empty()) { return rel; }
    }
    return fullPath;
}

std::wstring PlaylistIO::GenerateCollisionSafePath(const std::wstring& targetPath) {
    if (targetPath.empty()) return L"";
    if (!FileExists(targetPath)) return targetPath;
    size_t lastSlash = targetPath.find_last_of(L"\\/");
    std::wstring dir = (lastSlash != std::wstring::npos) ? targetPath.substr(0, lastSlash) : L".";
    std::wstring filename = (lastSlash != std::wstring::npos) ? targetPath.substr(lastSlash + 1) : targetPath;
    size_t dotPos = filename.find_last_of(L'.');
    std::wstring stem = (dotPos != std::wstring::npos) ? filename.substr(0, dotPos) : filename;
    std::wstring ext = (dotPos != std::wstring::npos) ? filename.substr(dotPos) : L"";
    for (int i = 1; i <= 9999; ++i) {
        std::wstring candidate = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
        if (!FileExists(candidate)) { return candidate; }
    }
    return targetPath;
}

std::string PlaylistIO::DecodeToUtf8(const char *data, size_t size, bool &outHasBom, std::string &outEncodingName) {
    outHasBom = false;
    outEncodingName = "UTF-8";
    if (!data || size == 0) return "";
    // 1. Check UTF-8 BOM: 0xEF, 0xBB, 0xBF
    if (size >= 3 && static_cast<unsigned char>(data[0]) == 0xEF && static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF)
    { outHasBom = true; outEncodingName = "UTF-8-BOM"; return std::string(data + 3, size - 3); }
    // 2. Check UTF-16LE BOM: 0xFF, 0xFE
    if (size >= 2 && static_cast<unsigned char>(data[0]) == 0xFF && static_cast<unsigned char>(data[1]) == 0xFE) {
        outHasBom = true;
        outEncodingName = "UTF-16LE";
        size_t wcharCount = (size - 2) / 2;
        std::wstring wstr(wcharCount, L'\0');
        std::memcpy(&wstr[0], data + 2, wcharCount * sizeof(wchar_t));
        return Utf16ToUtf8(wstr);
    }
    // 3. Check UTF-16BE BOM: 0xFE, 0xFF
    if (size >= 2 && static_cast<unsigned char>(data[0]) == 0xFE && static_cast<unsigned char>(data[1]) == 0xFF) {
        outHasBom = true;
        outEncodingName = "UTF-16BE";
        size_t wcharCount = (size - 2) / 2;
        std::wstring wstr(wcharCount, L'\0');
        for (size_t i = 0; i < wcharCount; ++i) {
            unsigned char b1 = static_cast<unsigned char>(data[2 + i * 2]);
            unsigned char b2 = static_cast<unsigned char>(data[2 + i * 2 + 1]);
            wstr[i] = static_cast<wchar_t>((static_cast<uint16_t>(b1) << 8) | static_cast<uint16_t>(b2));
        }
        return Utf16ToUtf8(wstr);
    }
    // 4. Validate UTF-8 without BOM
    if (IsValidUtf8(data, size)) { outHasBom = false; outEncodingName = "UTF-8"; return std::string(data, size); }
    // 5. Fallback: ANSI / Windows-1252 / CP_ACP
    outHasBom = false;
    outEncodingName = "ANSI";
    int reqLen = MultiByteToWideChar(CP_ACP, 0, data, static_cast<int>(size), nullptr, 0);
    if (reqLen > 0) {
        std::wstring wide(reqLen, L'\0');
        MultiByteToWideChar(CP_ACP, 0, data, static_cast<int>(size), &wide[0], reqLen);
        return Utf16ToUtf8(wide);
    }
    return std::string(data, size);
}

PlaylistParseResult PlaylistIO::ParseM3UContent( const std::string& utf8Content, const std::wstring& playlistDir,
    const PlaylistParseOptions &options) {
    PlaylistParseResult result;
    result.detectedFormat = PlaylistFormat::M3U8;
    std::istringstream stream(utf8Content);
    std::string line;
    size_t lineCount = 0;
    std::wstring pendingTitle;
    double pendingDuration = -1.0;
    std::string pendingRawMetadata;
    std::unordered_map<std::wstring, size_t> seenLocations;
    while (std::getline(stream, line)) {
        lineCount++;
        if (lineCount > options.maxLineCount) {
            return PlaylistParseResult::ErrorResult( PlaylistIoErrorCode::LineLimitExceeded, playlistDir,
                "M3U playlist exceeds maximum line count limit of " + std::to_string(options.maxLineCount));
        }
        if (line.size() > options.maxLineLength) {
            return PlaylistParseResult::ErrorResult( PlaylistIoErrorCode::LineTooLong, playlistDir,
                "Line " + std::to_string(lineCount) + " exceeds maximum allowed line length");
        }
        std::string trimmed = Trim(line);
        if (trimmed.empty()) { continue; }
        if (trimmed[0] == '#') {
            // Directive or comment
            if (StartsWithIgnoreCase(trimmed, "#EXTM3U")) {
                result.detectedFormat = PlaylistFormat::M3U8;
            } else if (StartsWithIgnoreCase(trimmed, "#EXTINF:")) {
                pendingRawMetadata = trimmed;
                // Parse #EXTINF:<duration>,<title>
                std::string rest = trimmed.substr(8);
                size_t commaPos = rest.find(',');
                if (commaPos != std::string::npos) {
                    std::string durStr = Trim(rest.substr(0, commaPos));
                    std::string titleStr = Trim(rest.substr(commaPos + 1));
                    try { pendingDuration = std::stod(durStr); } catch (...) { pendingDuration = -1.0; }
                    pendingTitle = Utf8ToUtf16(titleStr);
                } else {
                    std::string durStr = Trim(rest);
                    try { pendingDuration = std::stod(durStr); } catch (...) { pendingDuration = -1.0; }
                    pendingTitle = L"";
                }
            } else {
                // Other comment/directive
                if (pendingRawMetadata.empty()) {
                    pendingRawMetadata = trimmed;
                } else { pendingRawMetadata += "\n" + trimmed; }
            }
            continue;
        }
        // File path or URL
        std::wstring rawLocation = Utf8ToUtf16(trimmed);
        bool isUrl = IsUrl(rawLocation);
        std::wstring resolvedLocation = ResolveEntryLocation( rawLocation, playlistDir, options.resolveRelativePaths);
        PlaylistEntry entry;
        entry.location = resolvedLocation;
        entry.isUrl = isUrl;
        entry.title = pendingTitle;
        entry.durationSec = pendingDuration;
        entry.rawMetadata = pendingRawMetadata;
        if (entry.title.empty()) {
            // Extract filename as default title if not a URL
            if (!isUrl && !resolvedLocation.empty()) {
                size_t lastSlash = resolvedLocation.find_last_of(L"\\/");
                entry.title = (lastSlash != std::wstring::npos) ? resolvedLocation.substr(lastSlash + 1) : resolvedLocation;
            } else { entry.title = rawLocation; }
        }
        if (isUrl) { entry.fileExists = true; result.urlItemCount++; } else {
            if (options.checkFileExistence) {
                entry.fileExists = FileExists(resolvedLocation);
                if (!entry.fileExists) { result.missingItemCount++; } else { result.validItemCount++; }
            } else { entry.fileExists = true; result.validItemCount++; }
        }
        auto it = seenLocations.find(resolvedLocation);
        if (it != seenLocations.end()) {
            result.duplicateItemCount++;
            if (options.preserveDuplicates) { result.entries.push_back(std::move(entry)); }
        } else { seenLocations[resolvedLocation] = result.entries.size(); result.entries.push_back(std::move(entry)); }
        if (result.entries.size() > options.maxItemCount) {
            return PlaylistParseResult::ErrorResult( PlaylistIoErrorCode::ItemLimitExceeded, playlistDir,
                "M3U playlist exceeds maximum item limit of " + std::to_string(options.maxItemCount));
        }
        // Reset pending fields for next entry
        pendingTitle.clear();
        pendingDuration = -1.0;
        pendingRawMetadata.clear();
    }
    result.totalLinesParsed = lineCount;
    if (result.entries.empty() && !options.allowEmptyPlaylist) {
        return PlaylistParseResult::ErrorResult( PlaylistIoErrorCode::EmptyPlaylist, playlistDir,
            "M3U playlist contains no valid media entries");
    }
    result.success = true;
    result.errorCode = PlaylistIoErrorCode::Success;
    return result;
}

PlaylistParseResult PlaylistIO::ParsePLSContent( const std::string& utf8Content, const std::wstring& playlistDir,
    const PlaylistParseOptions &options) {
    PlaylistParseResult result;
    result.detectedFormat = PlaylistFormat::PLS;
    std::istringstream stream(utf8Content);
    std::string line;
    size_t lineCount = 0;
    bool foundSection = false;
    struct PlsRawItem { std::string file; std::string title; double length = -1.0; bool hasFile = false; };
    std::map<int, PlsRawItem> rawItems;
    int maxIndex = 0;
    while (std::getline(stream, line)) {
        lineCount++;
        if (lineCount > options.maxLineCount) {
            return PlaylistParseResult::ErrorResult( PlaylistIoErrorCode::LineLimitExceeded, playlistDir,
                "PLS playlist exceeds maximum line count limit of " + std::to_string(options.maxLineCount));
        }
        if (line.size() > options.maxLineLength) {
            return PlaylistParseResult::ErrorResult( PlaylistIoErrorCode::LineTooLong, playlistDir,
                "Line " + std::to_string(lineCount) + " exceeds maximum allowed line length");
        }
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') { continue; }
        if (trimmed[0] == '[' && trimmed.back() == ']') {
            std::string section = trimmed.substr(1, trimmed.size() - 2);
            if (EqualsIgnoreCase(section, "playlist")) { foundSection = true; }
            continue;
        }
        size_t eqPos = trimmed.find('=');
        if (eqPos == std::string::npos) { continue; }
        std::string key = Trim(trimmed.substr(0, eqPos));
        std::string value = Trim(trimmed.substr(eqPos + 1));
        if (StartsWithIgnoreCase(key, "file")) {
            std::string numStr = key.substr(4);
            try {
                int idx = std::stoi(numStr);
                rawItems[idx].file = value;
                rawItems[idx].hasFile = true;
                if (idx > maxIndex) maxIndex = idx;
            } catch (...) {}
        } else if (StartsWithIgnoreCase(key, "title")) {
            std::string numStr = key.substr(5);
            try { int idx = std::stoi(numStr); rawItems[idx].title = value; if (idx > maxIndex) maxIndex = idx; } catch (...) {}
        } else if (StartsWithIgnoreCase(key, "length")) {
            std::string numStr = key.substr(6);
            try { int idx = std::stoi(numStr); rawItems[idx].length = std::stod(value); if (idx > maxIndex) maxIndex = idx; } catch (...) {}
        }
    }
    result.totalLinesParsed = lineCount;
    if (!foundSection && rawItems.empty()) {
        return PlaylistParseResult::ErrorResult( PlaylistIoErrorCode::InvalidHeader, playlistDir,
            "PLS playlist missing [playlist] header and contains no valid entries");
    }
    std::unordered_map<std::wstring, size_t> seenLocations;
    for (const auto& pair : rawItems) {
        const auto& raw = pair.second;
        if (!raw.hasFile || raw.file.empty()) { continue; }
        std::wstring rawLocation = Utf8ToUtf16(raw.file);
        bool isUrl = IsUrl(rawLocation);
        std::wstring resolvedLocation = ResolveEntryLocation( rawLocation, playlistDir, options.resolveRelativePaths);
        PlaylistEntry entry;
        entry.location = resolvedLocation;
        entry.isUrl = isUrl;
        entry.durationSec = raw.length;
        if (!raw.title.empty()) { entry.title = Utf8ToUtf16(raw.title); } else {
            if (!isUrl && !resolvedLocation.empty()) {
                size_t lastSlash = resolvedLocation.find_last_of(L"\\/");
                entry.title = (lastSlash != std::wstring::npos) ? resolvedLocation.substr(lastSlash + 1) : resolvedLocation;
            } else { entry.title = rawLocation; }
        }
        if (isUrl) { entry.fileExists = true; result.urlItemCount++; } else {
            if (options.checkFileExistence) {
                entry.fileExists = FileExists(resolvedLocation);
                if (!entry.fileExists) { result.missingItemCount++; } else { result.validItemCount++; }
            } else { entry.fileExists = true; result.validItemCount++; }
        }
        auto it = seenLocations.find(resolvedLocation);
        if (it != seenLocations.end()) {
            result.duplicateItemCount++;
            if (options.preserveDuplicates) { result.entries.push_back(std::move(entry)); }
        } else { seenLocations[resolvedLocation] = result.entries.size(); result.entries.push_back(std::move(entry)); }
        if (result.entries.size() > options.maxItemCount) {
            return PlaylistParseResult::ErrorResult( PlaylistIoErrorCode::ItemLimitExceeded, playlistDir,
                "PLS playlist exceeds maximum item limit of " + std::to_string(options.maxItemCount));
        }
    }
    if (result.entries.empty() && !options.allowEmptyPlaylist) {
        return PlaylistParseResult::ErrorResult( PlaylistIoErrorCode::EmptyPlaylist, playlistDir,
            "PLS playlist contains no valid media entries");
    }
    result.success = true;
    result.errorCode = PlaylistIoErrorCode::Success;
    return result;
}

PlaylistParseResult PlaylistIO::ParseContent( const std::string& rawBytes, PlaylistFormat format, const std::wstring& playlistDir,
    const PlaylistParseOptions &options) {
    if (rawBytes.size() > options.maxFileSizeBytes) {
        return PlaylistParseResult::ErrorResult( PlaylistIoErrorCode::FileTooLarge, playlistDir,
            "Playlist content size (" + std::to_string(rawBytes.size()) +
            " bytes) exceeds maximum limit of " + std::to_string(options.maxFileSizeBytes) + " bytes");
    }
    bool hasBom = false;
    std::string encodingName;
    std::string utf8Content = DecodeToUtf8(rawBytes.data(), rawBytes.size(), hasBom, encodingName);
    if (format == PlaylistFormat::Auto) { format = DetectFormatFromContent(utf8Content); }
    if (format == PlaylistFormat::PLS) {
        return ParsePLSContent(utf8Content, playlistDir, options);
    } else { return ParseM3UContent(utf8Content, playlistDir, options); }
}

PlaylistParseResult PlaylistIO::ImportPlaylistFile(const std::wstring &filePath, const PlaylistParseOptions &options) {
    if (filePath.empty())
        return PlaylistParseResult::ErrorResult( PlaylistIoErrorCode::InvalidPath, filePath, "Playlist file path is empty");
    std::wstring extPath = EnsureExtendedPath(filePath);
    // Check file attributes and size using CreateFileW in strictly read-only mode
    HANDLE hFile = CreateFileW( extPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        PlaylistIoErrorCode code = (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? PlaylistIoErrorCode::FileNotFound
            : PlaylistIoErrorCode::FileOpenFailed;
        return PlaylistParseResult::ErrorResult( code, filePath,
            "Failed to open playlist file for reading (Win32 Error: " + std::to_string(err) + ")", err);
    }
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        DWORD err = GetLastError();
        CloseHandle(hFile);
        return PlaylistParseResult::ErrorResult( PlaylistIoErrorCode::FileReadFailed, filePath, "Failed to get playlist file size", err);
    }
    if (static_cast<size_t>(fileSize.QuadPart) > options.maxFileSizeBytes) {
        CloseHandle(hFile);
        return PlaylistParseResult::ErrorResult( PlaylistIoErrorCode::FileTooLarge, filePath,
            "Playlist file size exceeds maximum limit of " + std::to_string(options.maxFileSizeBytes) + " bytes");
    }
    if (fileSize.QuadPart == 0) {
        CloseHandle(hFile);
        if (!options.allowEmptyPlaylist)
            return PlaylistParseResult::ErrorResult( PlaylistIoErrorCode::EmptyPlaylist, filePath, "Playlist file is empty");
        return PlaylistParseResult::SuccessResult(filePath, PlaylistFormat::M3U8, {});
    }
    std::string rawBytes;
    rawBytes.resize(static_cast<size_t>(fileSize.QuadPart));
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, &rawBytes[0], static_cast<DWORD>(rawBytes.size()), &bytesRead, nullptr) ||
        bytesRead != static_cast<DWORD>(rawBytes.size())) {
        DWORD err = GetLastError();
        CloseHandle(hFile);
        return PlaylistParseResult::ErrorResult(PlaylistIoErrorCode::FileReadFailed, filePath, "Failed to read playlist file content", err);
    }
    CloseHandle(hFile);
    // Extract directory for relative path resolution
    std::wstring playlistDir;
    size_t lastSlash = filePath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) { playlistDir = filePath.substr(0, lastSlash); } else { playlistDir = L"."; }
    PlaylistFormat format = DetectFormatFromExtension(filePath);
    PlaylistParseResult result = ParseContent(rawBytes, format, playlistDir, options);
    result.sourcePath = filePath;
    // Set playlist title from filename if title empty
    if (result.playlistTitle.empty()) {
        std::wstring filename = (lastSlash != std::wstring::npos) ? filePath.substr(lastSlash + 1) : filePath;
        size_t dotPos = filename.find_last_of(L'.');
        result.playlistTitle = (dotPos != std::wstring::npos) ? filename.substr(0, dotPos) : filename;
    }
    return result;
}

std::string PlaylistIO::SerializeM3U(const std::vector<PlaylistEntry> &entries, const PlaylistExportOptions &options) {
    std::ostringstream ss;
    ss << "#EXTM3U\n";
    for (const auto& entry : entries) {
        if (entry.location.empty()) continue;
        if (options.writeExtendedMetadata) {
            int dur = (entry.durationSec >= 0.0) ? static_cast<int>(std::round(entry.durationSec)) : -1;
            std::string titleUtf8 = Utf16ToUtf8(entry.title);
            ss << "#EXTINF:" << dur << "," << titleUtf8 << "\n";
        }
        std::wstring outPath = entry.location;
        if (options.useRelativePathsWhereSafe && !entry.isUrl) { outPath = MakeRelativePathWhereSafe(entry.location, options.basePath); }
        ss << Utf16ToUtf8(outPath) << "\n";
    }
    return ss.str();
}

std::string PlaylistIO::SerializePLS(const std::vector<PlaylistEntry> &entries, const PlaylistExportOptions &options) {
    std::ostringstream ss;
    ss << "[playlist]\n";
    size_t count = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (entry.location.empty()) continue;
        count++;
        std::wstring outPath = entry.location;
        if (options.useRelativePathsWhereSafe && !entry.isUrl) { outPath = MakeRelativePathWhereSafe(entry.location, options.basePath); }
        ss << "File" << count << "=" << Utf16ToUtf8(outPath) << "\n";
        if (options.writeExtendedMetadata) {
            if (!entry.title.empty()) { ss << "Title" << count << "=" << Utf16ToUtf8(entry.title) << "\n"; }
            int dur = (entry.durationSec >= 0.0) ? static_cast<int>(std::round(entry.durationSec)) : -1;
            ss << "Length" << count << "=" << dur << "\n";
        }
    }
    ss << "NumberOfEntries=" << count << "\n";
    ss << "Version=2\n";
    return ss.str();
}

PlaylistExportResult PlaylistIO::ExportPlaylistFile( const std::wstring& destinationPath, const std::vector<PlaylistEntry>& entries,
    const PlaylistExportOptions &options) {
    if (destinationPath.empty())
        return PlaylistExportResult::ErrorResult( PlaylistIoErrorCode::InvalidPath, destinationPath, "Destination playlist path is empty");
    std::wstring finalPath = destinationPath;
    if (options.collisionSafe) { finalPath = GenerateCollisionSafePath(destinationPath); }
    PlaylistExportOptions opt = options;
    if (opt.basePath.empty()) {
        size_t lastSlash = finalPath.find_last_of(L"\\/");
        opt.basePath = (lastSlash != std::wstring::npos) ? finalPath.substr(0, lastSlash) : L".";
    }
    if (opt.format == PlaylistFormat::Auto) {
        opt.format = DetectFormatFromExtension(finalPath);
        if (opt.format == PlaylistFormat::Auto) { opt.format = PlaylistFormat::M3U8; }
    }
    std::string textContent;
    if (opt.format == PlaylistFormat::PLS) { textContent = SerializePLS(entries, opt); } else { textContent = SerializeM3U(entries, opt); }
    std::string binaryPayload;
    if (opt.writeUtf8Bom) { binaryPayload.push_back('\xEF'); binaryPayload.push_back('\xBB'); binaryPayload.push_back('\xBF'); }
    binaryPayload.append(textContent);
    std::wstring extFinalPath = EnsureExtendedPath(finalPath);
    // Create parent directory if needed
    size_t lastSlash = extFinalPath.find_last_of(L"\\/");
    std::wstring dir = (lastSlash != std::wstring::npos) ? extFinalPath.substr(0, lastSlash) : L".";
    DWORD dirAttr = GetFileAttributesW(dir.c_str());
    if (dirAttr == INVALID_FILE_ATTRIBUTES || !(dirAttr & FILE_ATTRIBUTE_DIRECTORY)) {
        int res = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        if (res != ERROR_SUCCESS && res != ERROR_ALREADY_EXISTS && res != ERROR_FILE_EXISTS) {
            DWORD err = GetLastError();
            return PlaylistExportResult::ErrorResult( PlaylistIoErrorCode::DirectoryCreationFailed, finalPath,
                "Failed to create export directory: " + Utf16ToUtf8(dir), err ? err : static_cast<DWORD>(res));
        }
    }
    if (opt.atomicWrite) {
        std::wstring tmpPath = extFinalPath + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetCurrentThreadId());
        HANDLE hFile = CreateFileW( tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            return PlaylistExportResult::ErrorResult( PlaylistIoErrorCode::FileOpenFailed, finalPath,
                "Failed to create temporary file for atomic write (Win32 Error: " + std::to_string(err) + ")", err);
        }
        DWORD written = 0;
        BOOL writeOk = TRUE;
        if (!binaryPayload.empty()) {
            writeOk = WriteFile(hFile, binaryPayload.data(), static_cast<DWORD>(binaryPayload.size()), &written, nullptr) &&
                      (written == static_cast<DWORD>(binaryPayload.size()));
        }
        DWORD writeErr = writeOk ? 0 : GetLastError();
        if (writeOk) { FlushFileBuffers(hFile); }
        CloseHandle(hFile);
        if (!writeOk) {
            DeleteFileW(tmpPath.c_str());
            return PlaylistExportResult::ErrorResult( PlaylistIoErrorCode::FileWriteFailed, finalPath,
                "Failed to write playlist payload to temporary file", writeErr);
        }
        if (!MoveFileExW(tmpPath.c_str(), extFinalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DWORD moveErr = GetLastError();
            DeleteFileW(tmpPath.c_str());
            return PlaylistExportResult::ErrorResult( PlaylistIoErrorCode::AtomicRenameFailed, finalPath,
                "Failed to rename temporary playlist to destination path", moveErr);
        }
    } else {
        HANDLE hFile = CreateFileW( extFinalPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            return PlaylistExportResult::ErrorResult( PlaylistIoErrorCode::FileOpenFailed, finalPath,
                "Failed to open destination file for writing", err);
        }
        DWORD written = 0;
        BOOL writeOk = TRUE;
        if (!binaryPayload.empty()) {
            writeOk = WriteFile(hFile, binaryPayload.data(), static_cast<DWORD>(binaryPayload.size()), &written, nullptr) &&
                      (written == static_cast<DWORD>(binaryPayload.size()));
        }
        DWORD writeErr = writeOk ? 0 : GetLastError();
        if (writeOk) { FlushFileBuffers(hFile); }
        CloseHandle(hFile);
        if (!writeOk) {
            return PlaylistExportResult::ErrorResult( PlaylistIoErrorCode::FileWriteFailed, finalPath,
                "Failed to write content to destination playlist file", writeErr);
        }
    }
    return PlaylistExportResult::SuccessResult(finalPath, opt.format, entries.size());
}

PlaylistExportResult PlaylistIO::ExportPlaylistFile( const std::wstring& destinationPath, const Playlist& playlist,
    const PlaylistExportOptions& options)
{ std::vector<PlaylistEntry> entries = FromMediaLibraryPlaylist(playlist); return ExportPlaylistFile(destinationPath, entries, options); }

PlaylistExportResult PlaylistIO::ExportPlaylistFile( const std::wstring& destinationPath, const std::vector<std::wstring>& itemPaths,
    const std::wstring &playlistTitle, const PlaylistExportOptions &options) {
    std::vector<PlaylistEntry> entries;
    entries.reserve(itemPaths.size());
    for (const auto& path : itemPaths) {
        PlaylistEntry e;
        e.location = path;
        e.isUrl = IsUrl(path);
        size_t lastSlash = path.find_last_of(L"\\/");
        e.title = (lastSlash != std::wstring::npos) ? path.substr(lastSlash + 1) : path;
        entries.push_back(std::move(e));
    }
    return ExportPlaylistFile(destinationPath, entries, options);
}

std::vector<PlaylistEntry> PlaylistIO::FromMediaLibraryPlaylist(const Playlist& playlist) {
    std::vector<PlaylistEntry> entries;
    entries.reserve(playlist.itemPaths.size());
    for (const auto& path : playlist.itemPaths) {
        PlaylistEntry e;
        e.location = path;
        e.isUrl = IsUrl(path);
        size_t lastSlash = path.find_last_of(L"\\/");
        e.title = (lastSlash != std::wstring::npos) ? path.substr(lastSlash + 1) : path;
        entries.push_back(std::move(e));
    }
    return entries;
}

} // namespace Aperture
