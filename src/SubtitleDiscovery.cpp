#include "SubtitleDiscovery.h"
#include "Json.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <sstream>
#include <fstream>
#include <unordered_map>

namespace Aperture {

namespace {

std::wstring ToLowerWide(std::wstring str) {
    for (wchar_t& c : str) { c = std::towlower(c); }
    return str;
}

std::string ToLowerAscii(std::string str) {
    for (char& c : str) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return str;
}

std::wstring TrimWide(const std::wstring& str) {
    size_t first = 0;
    while (first < str.size() && (str[first] == L' ' || str[first] == L'\t' || str[first] == L'\r' || str[first] == L'\n')) { ++first; }
    size_t last = str.size();
    while (last > first && (str[last - 1] == L' ' || str[last - 1] == L'\t' || str[last - 1] == L'\r' || str[last - 1] == L'\n')) {
        --last;
    }
    return str.substr(first, last - first);
}

bool IsSupportedSubtitleExtension(const std::wstring& ext) {
    std::wstring lower = ToLowerWide(ext);
    return lower == L".srt" || lower == L".vtt" || lower == L".ass" || lower == L".ssa" || lower == L".sub" || lower == L".smi" ||
           lower == L".sami" || lower == L".pjs" || lower == L".vplayer" || lower == L".lrc";
}

bool IsUtf8Valid(const uint8_t* data, size_t size, size_t& nonAsciiCount) {
    nonAsciiCount = 0;
    size_t i = 0;
    while (i < size) {
        uint8_t byte = data[i];
        if (byte <= 0x7F) {
            // Null bytes in the middle of plain text are suspicious for text subtitles
            ++i;
        } else if ((byte & 0xE0) == 0xC0) {
            if (i + 1 >= size) return false;
            if ((data[i + 1] & 0xC0) != 0x80) return false;
            if (byte < 0xC2) return false; // Overlong
            ++nonAsciiCount;
            i += 2;
        } else if ((byte & 0xF0) == 0xE0) {
            if (i + 2 >= size) return false;
            if ((data[i + 1] & 0xC0) != 0x80 || (data[i + 2] & 0xC0) != 0x80) return false;
            if (byte == 0xE0 && (data[i + 1] < 0xA0)) return false; // Overlong
            if (byte == 0xED && (data[i + 1] >= 0xA0)) return false; // UTF-16 surrogates
            ++nonAsciiCount;
            i += 3;
        } else if ((byte & 0xF8) == 0xF0) {
            if (i + 3 >= size) return false;
            if ((data[i + 1] & 0xC0) != 0x80 || (data[i + 2] & 0xC0) != 0x80 || (data[i + 3] & 0xC0) != 0x80) return false;
            if (byte == 0xF0 && (data[i + 1] < 0x90)) return false; // Overlong
            if (byte == 0xF4 && (data[i + 1] > 0x8F)) return false; // > U+10FFFF
            if (byte > 0xF4) return false;
            ++nonAsciiCount;
            i += 4;
        } else { return false; }
    }
    return true;
}

} // namespace

int SubtitleDiscovery::ClampDelayMs(int delayMs) { return std::clamp(delayMs, kMinSubtitleDelayMs, kMaxSubtitleDelayMs); }

bool SubtitleDiscovery::IsReservedDeviceName(const std::wstring& pathOrName) {
    if (pathOrName.empty()) return false;
    std::wstring name = GetFileNameFromPath(pathOrName);
    size_t dot = name.find(L'.');
    std::wstring stem = (dot != std::wstring::npos) ? name.substr(0, dot) : name;
    std::wstring lower = ToLowerWide(TrimWide(stem));
    if (lower == L"con" || lower == L"prn" || lower == L"aux" || lower == L"nul" ||
        lower == L"clock$" || lower == L"conin$" || lower == L"conout$") {
        return true;
    }
    if (lower.size() == 4) {
        if ((lower.rfind(L"com", 0) == 0 || lower.rfind(L"lpt", 0) == 0) && (lower[3] >= L'1' && lower[3] <= L'9')) { return true; }
    }
    return false;
}

std::wstring SubtitleDiscovery::NormalizePath(const std::wstring& path) {
    std::wstring trimmed = TrimWide(path);
    if (trimmed.empty()) return L"";
    // Reject control characters
    for (wchar_t ch : trimmed) {
        if (ch < 32 && ch != L'\t') return L"";
        if (ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') return L"";
    }
    // Convert forward slashes to backslashes
    for (wchar_t& ch : trimmed) { if (ch == L'/') ch = L'\\'; }
    bool hasExtendedPrefix = (trimmed.rfind(L"\\\\?\\", 0) == 0);
    std::wstring working = hasExtendedPrefix ? trimmed.substr(4) : trimmed;
    // Check for reserved DOS devices
    if (IsReservedDeviceName(working)) return L"";
    // Split components and resolve . and ..
    std::vector<std::wstring> parts;
    std::wstring drivePrefix;
    if (working.size() >= 2 && working[1] == L':' &&
        ((working[0] >= L'A' && working[0] <= L'Z') || (working[0] >= L'a' && working[0] <= L'a' + 25))) {
        drivePrefix = working.substr(0, 2);
        working = working.substr(2);
    } else if (working.rfind(L"\\\\", 0) == 0) {
        // UNC path
        size_t nextSlash = working.find(L'\\', 2);
        if (nextSlash != std::wstring::npos) {
            size_t shareSlash = working.find(L'\\', nextSlash + 1);
            if (shareSlash != std::wstring::npos) {
                drivePrefix = working.substr(0, shareSlash);
                working = working.substr(shareSlash);
            } else { drivePrefix = working; working = L""; }
        }
    }
    std::wstring token;
    std::wistringstream tokenStream(working);
    while (std::getline(tokenStream, token, L'\\')) {
        if (token.empty() || token == L".") { continue; }
        if (token == L"..") {
            if (!parts.empty()) { parts.pop_back(); }
        } else { if (IsReservedDeviceName(token)) return L""; parts.push_back(token); }
    }
    std::wstring result = drivePrefix;
    if (result.empty() && !trimmed.empty() && trimmed[0] == L'\\') { result = L"\\"; }
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!result.empty() && result.back() != L'\\') { result += L'\\'; }
        result += parts[i];
    }
    return result;
}

bool SubtitleDiscovery::IsPathContained(const std::wstring& basePath, const std::wstring& candidatePath) {
    std::wstring normBase = NormalizePath(basePath);
    std::wstring normCandidate = NormalizePath(candidatePath);
    if (normBase.empty() || normCandidate.empty()) return false;
    if (normBase.back() != L'\\') { normBase += L'\\'; }
    std::wstring lowerBase = ToLowerWide(normBase);
    std::wstring lowerCandidate = ToLowerWide(normCandidate);
    if (lowerCandidate.rfind(lowerBase, 0) == 0) { return true; }
    return false;
}

SubtitleEncoding SubtitleDiscovery::DetectEncoding(const uint8_t* data, size_t size) {
    if (!data || size == 0) return SubtitleEncoding::Unknown;
    // 1. Check BOMs
    if (size >= 4) {
        if (data[0] == 0xFF && data[1] == 0xFE && data[2] == 0x00 && data[3] == 0x00) { return SubtitleEncoding::Utf32LE; }
        if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0xFE && data[3] == 0xFF) { return SubtitleEncoding::Utf32BE; }
    }
    if (size >= 3) {
        if (data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) { return SubtitleEncoding::Utf8Bom; }
    }
    if (size >= 2) {
        if (data[0] == 0xFF && data[1] == 0xFE) { return SubtitleEncoding::Utf16LE; }
        if (data[0] == 0xFE && data[1] == 0xFF) { return SubtitleEncoding::Utf16BE; }
    }
    // 2. Check UTF-16 heuristics without BOM (alternating zero bytes)
    if (size >= 8) {
        size_t sampleSize = std::min<size_t>(size, 4096);
        size_t zeroOdd = 0, zeroEven = 0, asciiOdd = 0, asciiEven = 0;
        size_t pairs = sampleSize / 2;
        for (size_t i = 0; i < pairs; ++i) {
            uint8_t b0 = data[2 * i];
            uint8_t b1 = data[2 * i + 1];
            if (b1 == 0 && (b0 >= 0x09 && b0 <= 0x7E)) { ++zeroOdd; ++asciiEven; }
            if (b0 == 0 && (b1 >= 0x09 && b1 <= 0x7E)) { ++zeroEven; ++asciiOdd; }
        }
        if (pairs > 4) {
            if (zeroOdd * 100 / pairs >= 60) { return SubtitleEncoding::Utf16LE; }
            if (zeroEven * 100 / pairs >= 60) { return SubtitleEncoding::Utf16BE; }
        }
    }
    // 3. Check UTF-8 validity
    size_t nonAscii = 0;
    if (IsUtf8Valid(data, size, nonAscii)) { return SubtitleEncoding::Utf8; }
    // 4. Fallback to ANSI
    return SubtitleEncoding::Ansi;
}

SubtitleEncoding SubtitleDiscovery::DetectEncoding(const std::string& bytes) {
    return DetectEncoding(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

std::string SubtitleDiscovery::DecodeSubtitleBytes(const std::string& bytes, SubtitleEncoding* outEncoding) {
    if (bytes.empty() || bytes.size() > kMaxSubtitleInputBytes) { if (outEncoding) *outEncoding = SubtitleEncoding::Unknown; return ""; }
    SubtitleEncoding encoding = DetectEncoding(bytes);
    if (outEncoding) *outEncoding = encoding;
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(bytes.data());
    const size_t len = bytes.size();
    switch (encoding) {
        case SubtitleEncoding::Utf8Bom: { std::string utf8 = bytes.substr(3); return utf8; }
        case SubtitleEncoding::Utf16LE: {
            size_t offset = (len >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) ? 2 : 0;
            std::wstring wide;
            wide.reserve((len - offset) / 2);
            for (size_t i = offset; i + 1 < len; i += 2) {
                wchar_t ch = static_cast<wchar_t>(raw[i] | (raw[i + 1] << 8));
                if (ch != L'\0') wide.push_back(ch);
            }
            return Utf16ToUtf8(wide);
        }
        case SubtitleEncoding::Utf16BE: {
            size_t offset = (len >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) ? 2 : 0;
            std::wstring wide;
            wide.reserve((len - offset) / 2);
            for (size_t i = offset; i + 1 < len; i += 2) {
                wchar_t ch = static_cast<wchar_t>((raw[i] << 8) | raw[i + 1]);
                if (ch != L'\0') wide.push_back(ch);
            }
            return Utf16ToUtf8(wide);
        }
        case SubtitleEncoding::Utf32LE: {
            size_t offset = (len >= 4 && raw[0] == 0xFF && raw[1] == 0xFE && raw[2] == 0x00 && raw[3] == 0x00) ? 4 : 0;
            std::wstring wide;
            wide.reserve((len - offset) / 4);
            for (size_t i = offset; i + 3 < len; i += 4) {
                uint32_t cp = raw[i] | (raw[i + 1] << 8) | (raw[i + 2] << 16) | (raw[i + 3] << 24);
                if (cp <= 0xFFFF && cp != 0) { wide.push_back(static_cast<wchar_t>(cp)); } else if (cp > 0xFFFF && cp <= 0x10FFFF) {
                    cp -= 0x10000;
                    wide.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
                    wide.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
                }
            }
            return Utf16ToUtf8(wide);
        }
        case SubtitleEncoding::Utf32BE: {
            size_t offset = (len >= 4 && raw[0] == 0x00 && raw[1] == 0x00 && raw[2] == 0xFE && raw[3] == 0xFF) ? 4 : 0;
            std::wstring wide;
            wide.reserve((len - offset) / 4);
            for (size_t i = offset; i + 3 < len; i += 4) {
                uint32_t cp = (raw[i] << 24) | (raw[i + 1] << 16) | (raw[i + 2] << 8) | raw[i + 3];
                if (cp <= 0xFFFF && cp != 0) { wide.push_back(static_cast<wchar_t>(cp)); } else if (cp > 0xFFFF && cp <= 0x10FFFF) {
                    cp -= 0x10000;
                    wide.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
                    wide.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
                }
            }
            return Utf16ToUtf8(wide);
        }
        case SubtitleEncoding::Utf8: { return bytes; }
        case SubtitleEncoding::Ansi:
        default: {
            int wideLength = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (wideLength > 0) {
                std::wstring wide(static_cast<size_t>(wideLength), L'\0');
                MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), &wide[0], wideLength);
                return Utf16ToUtf8(wide);
            }
            return bytes;
        }
    }
}

std::wstring SubtitleDiscovery::ParseLanguageFromFileName(const std::wstring& fileName) {
    if (fileName.empty()) return L"";
    std::wstring lower = ToLowerWide(fileName);
    // Common ISO 639-1 / 639-2 language code and tag mappings
    static const std::unordered_map<std::wstring, std::wstring> langMap = {
        { L"en", L"English" }, { L"eng", L"English" }, { L"english", L"English" },
        { L"es", L"Spanish" }, { L"spa", L"Spanish" }, { L"spanish", L"Spanish" },
        { L"fr", L"French" }, { L"fra", L"French" }, { L"fre", L"French" }, { L"french", L"French" },
        { L"de", L"German" }, { L"deu", L"German" }, { L"ger", L"German" }, { L"german", L"German" },
        { L"it", L"Italian" }, { L"ita", L"Italian" }, { L"italian", L"Italian" },
        { L"pt", L"Portuguese" }, { L"por", L"Portuguese" }, { L"portuguese", L"Portuguese" },
        { L"ru", L"Russian" }, { L"rus", L"Russian" }, { L"russian", L"Russian" },
        { L"ja", L"Japanese" }, { L"jpn", L"Japanese" }, { L"japanese", L"Japanese" },
        { L"zh", L"Chinese" }, { L"chi", L"Chinese" }, { L"zho", L"Chinese" }, { L"chinese", L"Chinese" },
        { L"ko", L"Korean" }, { L"kor", L"Korean" }, { L"korean", L"Korean" },
        { L"ar", L"Arabic" }, { L"ara", L"Arabic" }, { L"arabic", L"Arabic" },
        { L"hi", L"Hindi" }, { L"hin", L"Hindi" }, { L"hindi", L"Hindi" },
        { L"tr", L"Turkish" }, { L"tur", L"Turkish" }, { L"turkish", L"Turkish" },
        { L"nl", L"Dutch" }, { L"nld", L"Dutch" }, { L"dut", L"Dutch" }, { L"dutch", L"Dutch" },
        { L"pl", L"Polish" }, { L"pol", L"Polish" }, { L"polish", L"Polish" },
        { L"sv", L"Swedish" }, { L"swe", L"Swedish" }, { L"swedish", L"Swedish" },
        { L"da", L"Danish" }, { L"dan", L"Danish" }, { L"danish", L"Danish" },
        { L"no", L"Norwegian" }, { L"nor", L"Norwegian" }, { L"norwegian", L"Norwegian" },
        { L"fi", L"Finnish" }, { L"fin", L"Finnish" }, { L"finnish", L"Finnish" },
        { L"el", L"Greek" }, { L"ell", L"Greek" }, { L"gre", L"Greek" }, { L"greek", L"Greek" },
        { L"he", L"Hebrew" }, { L"heb", L"Hebrew" }, { L"hebrew", L"Hebrew" },
        { L"th", L"Thai" }, { L"tha", L"Thai" }, { L"thai", L"Thai" },
        { L"vi", L"Vietnamese" }, { L"vie", L"Vietnamese" }, { L"vietnamese", L"Vietnamese" },
        { L"id", L"Indonesian" }, { L"ind", L"Indonesian" }, { L"indonesian", L"Indonesian" },
        { L"uk", L"Ukrainian" }, { L"ukr", L"Ukrainian" }, { L"ukrainian", L"Ukrainian" },
        { L"cs", L"Czech" }, { L"ces", L"Czech" }, { L"cze", L"Czech" }, { L"czech", L"Czech" },
        { L"hu", L"Hungarian" }, { L"hun", L"Hungarian" }, { L"hungarian", L"Hungarian" },
        { L"ro", L"Romanian" }, { L"ron", L"Romanian" }, { L"rum", L"Romanian" }, { L"romanian", L"Romanian" }
    };
    // Extract tokens delimited by '.', '_', '-', ' ', '[', ']', '(', ')'
    std::vector<std::wstring> tokens;
    std::wstring currentToken;
    for (wchar_t ch : lower) {
        if (ch == L'.' || ch == L'_' || ch == L'-' || ch == L' ' || ch == L'[' || ch == L']' || ch == L'(' || ch == L')') {
            if (!currentToken.empty()) { tokens.push_back(currentToken); currentToken.clear(); }
        } else { currentToken.push_back(ch); }
    }
    if (!currentToken.empty()) { tokens.push_back(currentToken); }
    std::wstring recognizedLang;
    bool isForced = false, isSdh = false, isCc = false;
    for (const auto& tok : tokens) {
        if (tok == L"forced") { isForced = true; continue; }
        if (tok == L"sdh") { isSdh = true; continue; }
        if (tok == L"cc") { isCc = true; continue; }
        auto it = langMap.find(tok);
        if (it != langMap.end() && recognizedLang.empty()) { recognizedLang = it->second; }
    }
    if (recognizedLang.empty()) { if (isForced) return L"Forced"; if (isSdh) return L"SDH"; if (isCc) return L"CC"; return L""; }
    std::wstring result = recognizedLang;
    if (isForced) result += L" (Forced)";
    if (isSdh) result += L" (SDH)";
    if (isCc) result += L" (CC)";
    return result;
}

int SubtitleDiscovery::CalculateMatchScore(const std::wstring& mediaFileName, const std::wstring& subtitleFileName, bool inSubfolder) {
    std::wstring mediaStem = GetFileTitleFromPath(mediaFileName);
    std::wstring subStem = GetFileTitleFromPath(subtitleFileName);
    std::wstring lowerMedia = ToLowerWide(mediaStem);
    std::wstring lowerSub = ToLowerWide(subStem);
    // Exact stem match: movie.mkv <-> movie.srt (or subs/movie.srt)
    if (lowerMedia == lowerSub) { return inSubfolder ? 95 : 100; }
    // Suffix match: movie.en.srt, movie.eng.srt, movie-forced.srt
    if (lowerSub.rfind(lowerMedia, 0) == 0 && lowerSub.size() > lowerMedia.size()) {
        wchar_t sep = lowerSub[lowerMedia.size()];
        if (sep == L'.' || sep == L'_' || sep == L'-' || sep == L' ') { return inSubfolder ? 85 : 90; }
    }
    // In subfolder: check language or track names (e.g. subs/english.srt, subs/2_English.srt)
    if (inSubfolder) {
        std::wstring lang = ParseLanguageFromFileName(subtitleFileName);
        if (!lang.empty()) { return 75; }
        return 60;
    }
    // Sibling subtitle with recognized language
    std::wstring lang = ParseLanguageFromFileName(subtitleFileName);
    if (!lang.empty()) { return 40; }
    return 20;
}

std::vector<DiscoveredSubtitle> SubtitleDiscovery::DiscoverSubtitles(const std::wstring& mediaPath) {
    std::vector<DiscoveredSubtitle> results;
    std::wstring normMediaPath = NormalizePath(mediaPath);
    if (normMediaPath.empty()) return results;
    size_t lastSlash = normMediaPath.find_last_of(L"\\/");
    std::wstring mediaDir = (lastSlash != std::wstring::npos) ? normMediaPath.substr(0, lastSlash) : L"";
    if (mediaDir.empty()) return results;
    std::wstring mediaFileName = GetFileNameFromPath(normMediaPath);
    // 1. Scan immediate media directory (depth 0)
    auto scanDirectory = [&](const std::wstring& dirPath, bool isSubfolder, const std::wstring& subfolderLabel) {
        if (!IsPathContained(mediaDir, dirPath) && dirPath != mediaDir) { return; }
        WIN32_FIND_DATAW findData;
        std::wstring searchPattern = dirPath;
        if (searchPattern.back() != L'\\') searchPattern += L'\\';
        searchPattern += L"*.*";
        HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) return;
        do {
            if (results.size() >= kMaxDiscoveredFiles) break;
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue; // Do not recurse into deeper folders
            }
            std::wstring fileName = findData.cFileName;
            if (fileName == L"." || fileName == L"..") continue;
            std::wstring ext = GetFileExtension(fileName);
            if (!IsSupportedSubtitleExtension(ext)) continue;
            std::wstring fullPath = dirPath;
            if (fullPath.back() != L'\\') fullPath += L'\\';
            fullPath += fileName;
            if (!IsPathContained(mediaDir, fullPath)) continue;
            if (IsReservedDeviceName(fullPath)) continue;
            uint64_t fileSize = (static_cast<uint64_t>(findData.nFileSizeHigh) << 32) | findData.nFileSizeLow;
            if (fileSize > kMaxSubtitleInputBytes) continue; // Reject oversized files
            // Read file header to detect encoding
            SubtitleEncoding enc = SubtitleEncoding::Unknown;
            HANDLE hFile = CreateFileW(fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                uint8_t buffer[1024];
                DWORD bytesRead = 0;
                if (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0)
                    enc = DetectEncoding(buffer, static_cast<size_t>(bytesRead));
                CloseHandle(hFile);
            }
            DiscoveredSubtitle sub;
            sub.filePath = fullPath;
            sub.isSubfolder = isSubfolder;
            sub.fileSizeBytes = fileSize;
            sub.encoding = enc;
            sub.matchScore = CalculateMatchScore(mediaFileName, fileName, isSubfolder);
            sub.language = ParseLanguageFromFileName(fileName);
            std::wstring displayTitle;
            if (!sub.language.empty()) {
                displayTitle = sub.language;
                if (isSubfolder) { displayTitle += L" (" + subfolderLabel + L"/" + fileName + L")"; }
            } else {
                displayTitle = fileName;
                if (isSubfolder) { displayTitle = subfolderLabel + L"/" + fileName; }
            }
            sub.title = displayTitle;
            results.push_back(std::move(sub));
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    };
    std::vector<std::wstring> scannedDirs;
    scannedDirs.push_back(ToLowerWide(mediaDir));
    scanDirectory(mediaDir, false, L"");
    // Scan conventional immediate subfolders (depth 1 only): "subs", "subtitles"
    const wchar_t* subfolderNames[] = { L"subs", L"subtitles" };
    for (const wchar_t* subName : subfolderNames) {
        std::wstring subDir = mediaDir;
        if (subDir.back() != L'\\') subDir += L'\\';
        subDir += subName;
        std::wstring lowerSub = ToLowerWide(NormalizePath(subDir));
        bool alreadyScanned = false;
        for (const auto& s : scannedDirs) {
            if (s == lowerSub) { alreadyScanned = true; break; }
        }
        if (!alreadyScanned) {
            scannedDirs.push_back(lowerSub);
            DWORD attr = GetFileAttributesW(subDir.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) { scanDirectory(subDir, true, subName); }
        }
    }
    // Sort by match score descending, then by title
    std::stable_sort(results.begin(), results.end(), [](const DiscoveredSubtitle& a, const DiscoveredSubtitle& b) {
        if (a.matchScore != b.matchScore) return a.matchScore > b.matchScore;
        if (a.language != b.language) return a.language < b.language;
        return a.title < b.title;
    });
    return results;
}

std::wstring SubtitleDiscovery::ToRootRelativePath(const std::wstring& fullPath, const std::wstring& rootDir) {
    if (fullPath.empty()) return L"";
    std::wstring normFull = NormalizePath(fullPath);
    std::wstring normRoot = NormalizePath(rootDir);
    if (normRoot.empty() || normFull.empty()) return normFull;
    if (normRoot.back() != L'\\') normRoot += L'\\';
    std::wstring lowerFull = ToLowerWide(normFull);
    std::wstring lowerRoot = ToLowerWide(normRoot);
    if (lowerFull.rfind(lowerRoot, 0) == 0) { return normFull.substr(normRoot.size()); }
    return normFull;
}

std::wstring SubtitleDiscovery::FromRootRelativePath(const std::wstring& relPath, const std::wstring& rootDir) {
    if (relPath.empty()) return L"";
    // If already absolute (e.g. C:\... or \\...)
    if ((relPath.size() >= 2 && relPath[1] == L':') || relPath.rfind(L"\\\\", 0) == 0 || relPath.rfind(L"//", 0) == 0)
        return NormalizePath(relPath);
    std::wstring normRoot = NormalizePath(rootDir);
    if (normRoot.empty()) return NormalizePath(relPath);
    std::wstring combined = normRoot;
    if (combined.back() != L'\\') combined += L'\\';
    combined += relPath;
    std::wstring normalized = NormalizePath(combined);
    // Path containment check
    if (!IsPathContained(normRoot, normalized)) {
        return L""; // Reject escaping relative paths
    }
    return normalized;
}

std::string SubtitlePreferences::SerializeJson() const {
    JsonValue root = JsonValue::Object();
    root["primaryTrackIndex"] = primaryTrackIndex;
    root["secondaryTrackIndex"] = secondaryTrackIndex;
    root["primaryTrackPath"] = Utf16ToUtf8(primaryTrackPath);
    root["secondaryTrackPath"] = Utf16ToUtf8(secondaryTrackPath);
    root["primaryDelayMs"] = SubtitleDiscovery::ClampDelayMs(primaryDelayMs);
    root["secondaryDelayMs"] = SubtitleDiscovery::ClampDelayMs(secondaryDelayMs);
    root["primaryVisible"] = primaryVisible;
    root["secondaryVisible"] = secondaryVisible;
    root["primaryPosition"] = primaryPosition;
    root["secondaryPosition"] = secondaryPosition;
    root["primaryFontSize"] = primaryFontSize;
    root["secondaryFontSize"] = secondaryFontSize;
    root["primaryColor"] = primaryColor;
    root["secondaryColor"] = secondaryColor;
    return root.stringify();
}

bool SubtitlePreferences::DeserializeJson(const std::string& jsonStr) {
    if (jsonStr.empty()) return false;
    JsonValue root = JsonValue::parse(jsonStr);
    if (!root.is_object()) return false;
    if (root.has_key("primaryTrackIndex") && root.get("primaryTrackIndex").is_number())
        primaryTrackIndex = root.get("primaryTrackIndex").as_int();
    if (root.has_key("secondaryTrackIndex") && root.get("secondaryTrackIndex").is_number())
        secondaryTrackIndex = root.get("secondaryTrackIndex").as_int();
    if (root.has_key("primaryTrackPath") && root.get("primaryTrackPath").is_string())
        primaryTrackPath = Utf8ToUtf16(root.get("primaryTrackPath").as_string());
    if (root.has_key("secondaryTrackPath") && root.get("secondaryTrackPath").is_string())
        secondaryTrackPath = Utf8ToUtf16(root.get("secondaryTrackPath").as_string());
    if (root.has_key("primaryDelayMs") && root.get("primaryDelayMs").is_number())
        primaryDelayMs = SubtitleDiscovery::ClampDelayMs(root.get("primaryDelayMs").as_int());
    if (root.has_key("secondaryDelayMs") && root.get("secondaryDelayMs").is_number())
        secondaryDelayMs = SubtitleDiscovery::ClampDelayMs(root.get("secondaryDelayMs").as_int());
    if (root.has_key("primaryVisible") && root.get("primaryVisible").is_bool()) { primaryVisible = root.get("primaryVisible").as_bool(); }
    if (root.has_key("secondaryVisible") && root.get("secondaryVisible").is_bool())
        secondaryVisible = root.get("secondaryVisible").as_bool();
    if (root.has_key("primaryPosition") && root.get("primaryPosition").is_string())
        primaryPosition = root.get("primaryPosition").as_string();
    if (root.has_key("secondaryPosition") && root.get("secondaryPosition").is_string())
        secondaryPosition = root.get("secondaryPosition").as_string();
    if (root.has_key("primaryFontSize") && root.get("primaryFontSize").is_string())
        primaryFontSize = root.get("primaryFontSize").as_string();
    if (root.has_key("secondaryFontSize") && root.get("secondaryFontSize").is_string())
        secondaryFontSize = root.get("secondaryFontSize").as_string();
    if (root.has_key("primaryColor") && root.get("primaryColor").is_string()) { primaryColor = root.get("primaryColor").as_string(); }
    if (root.has_key("secondaryColor") && root.get("secondaryColor").is_string())
        secondaryColor = root.get("secondaryColor").as_string();
    return true;
}

} // namespace Aperture
